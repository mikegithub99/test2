# Kubernetes gRPC server discovery and targeted job operations

## Purpose

This document describes how to run multiple gRPC server instances of the same
server type in Kubernetes while preserving the existing Redis job ownership
and lifecycle model.

The recommended approach is:

- Run the server instances as a Kubernetes **StatefulSet**.
- Give the StatefulSet a **headless Service** so each pod has its own stable DNS
  name.
- Continue generating a globally unique `serverId` when each server process
  starts.
- Register the `serverId` and that process's gRPC target separately through
  the existing server-registration flow.
- Route cancel and delete requests to the endpoint registered for the job's
  owning `serverId`.

This is a deployment and registration design. It does not require the C++
server to read Kubernetes YAML files or to depend directly on the Kubernetes
API.

## Identity and connectivity are separate

Keep these concepts distinct:

- **`serverType`** identifies the logical category of server, such as
  `processor`.
- **`serverId`** identifies one running server process. The server continues
  to generate a globally unique ID at startup, as it does today.
- **`grpcTarget`** is the network address clients use to connect to that
  process, for example
  `job-server-1.job-server-headless.jobs.svc.cluster.local:50051`.

Do not use the pod name or DNS name as the `serverId`. A StatefulSet pod can
keep the same pod name and DNS name when Kubernetes replaces it, while the
replacement process should generate a new globally unique `serverId`.
Consequently, a stale request addressed to the previous process identity can
be rejected rather than being mistaken for a request to the replacement.

The gRPC port can be the same in every pod. Each pod has its own network
namespace and address, so all instances can listen on (for example) port
`50051`.

## StatefulSet DNS

Assume the following example configuration:

| Setting | Example |
| --- | --- |
| Namespace | `jobs` |
| StatefulSet | `job-server` |
| Headless Service | `job-server-headless` |
| gRPC port | `50051` |

The per-pod DNS names follow this pattern:

```text
<pod-name>.<headless-service>.<namespace>.svc.cluster.local:<port>
```

For example:

```text
job-server-0.job-server-headless.jobs.svc.cluster.local:50051
job-server-1.job-server-headless.jobs.svc.cluster.local:50051
job-server-2.job-server-headless.jobs.svc.cluster.local:50051
```

The StatefulSet's `serviceName` should refer to the headless Service. The
Service selector must select the StatefulSet pods, and its port should map to
the gRPC container port. The headless Service has `clusterIP: None`; it
provides DNS records for the individual pods instead of a single
load-balancing virtual IP.

A normal ClusterIP Service is not a substitute for this per-instance target
when sending a request to a particular `serverId`: it can route a connection
to any eligible pod, not necessarily the pod that owns the job.

## Supplying pod information to the C++ process

The C++ executable does not need to parse YAML. The Kubernetes workload
configuration supplies environment variables to the container. Kubernetes'
Downward API provides the pod name and namespace:

| Variable | Value source / example | Purpose |
| --- | --- | --- |
| `POD_NAME` | `metadata.name` | StatefulSet pod name |
| `POD_NAMESPACE` | `metadata.namespace` | Namespace containing the pod |
| `GRPC_SERVICE_NAME` | `job-server-headless` | Headless Service name |
| `GRPC_PORT` | `50051` | Port on which this process listens |

The server constructs its advertised target at startup:

```cpp
#include <cstdlib>
#include <stdexcept>
#include <string>

std::string getGrpcTarget()
{
    const char* podName = std::getenv("POD_NAME");
    const char* podNamespace = std::getenv("POD_NAMESPACE");
    const char* serviceName = std::getenv("GRPC_SERVICE_NAME");
    const char* grpcPort = std::getenv("GRPC_PORT");

    if (!podName || !podNamespace || !serviceName || !grpcPort ||
        *podName == '\\0' || *podNamespace == '\\0' ||
        *serviceName == '\\0' || *grpcPort == '\\0')
    {
        throw std::runtime_error(
            "Missing required gRPC endpoint environment configuration");
    }

    return std::string(podName) + "." +
           serviceName + "." +
           podNamespace +
           ".svc.cluster.local:" + grpcPort;
}
```

The server should bind its gRPC listener to `0.0.0.0:<GRPC_PORT>` (or the
appropriate all-interface address for the gRPC server implementation), not
only to `localhost`. The advertised target is the DNS name clients use; the
listener port and advertised port must agree.

The environment variables are supplied by the Kubernetes deployment
configuration. They can also be supplied by another runtime environment, so
the C++ application remains independent of Kubernetes.

## Registration metadata

At startup, the server continues to generate its unique `serverId`, builds
its `grpcTarget` from the environment, and registers both values:

```cpp
const std::string serverId = generateGlobalServerId();
const std::string grpcTarget = getGrpcTarget();

// Illustrative: adapt the arguments to the existing registration API.
redisJobStatus.registerServer(serverType, serverId, grpcTarget);
```

The example is illustrative; it does not prescribe a new C++ method
signature. Store the endpoint as registration metadata alongside the
existing server record, using the existing registration lifecycle where
possible. Keep `serverId` and `grpcTarget` as separate fields.

A registration record conceptually contains:

| Field | Example |
| --- | --- |
| `serverType` | `processor` |
| `serverId` | globally unique ID generated by this process |
| `grpcTarget` | `job-server-1.job-server-headless.jobs.svc.cluster.local:50051` |
| `podName` | `job-server-1` |
| `namespace` | `jobs` |

The endpoint is a locator, not an identity. Do not derive the server identity
from the endpoint.

## Routing cancel and delete requests

The job's existing ownership metadata should continue to identify the owning
`serverId`. To cancel or delete a job:

1. The requester obtains the job's owning `serverId`.
2. It resolves that ID to the currently registered `grpcTarget`.
3. It connects to that exact target and sends the operation with both
   `serverId` and `jobId`.
4. The receiving server compares the request's `serverId` with its own
   current process `serverId` and verifies that the job belongs to it.
5. If the identity and ownership checks pass, the server performs the
   requested operation. If they do not, it rejects the request without
   affecting a job.

For example, the request messages can carry the expected owner identity:

```protobuf
message CancelJobRequest {
  string server_id = 1;
  string job_id = 2;
  string request_id = 3;
}

message DeleteJobRequest {
  string server_id = 1;
  string job_id = 2;
  string request_id = 3;
}
```

The `request_id` is useful for tracing and deduplicating retries. The
receiving server must not trust the request's `server_id` alone: it should
compare it with its own current ID and confirm the job's ownership before
acting.

A mismatch should produce an explicit stale-owner / wrong-server response.
A missing or unreachable endpoint should be treated as a routing failure;
the requester can re-read registration state rather than sending the request
through a generic load-balanced Service.

## Replacement pods and the existing cleanup lifecycle

When Kubernetes replaces a pod:

- The replacement can have the same StatefulSet pod name and therefore the
  same per-pod DNS name.
- The replacement process generates a **new** globally unique `serverId`.
- It registers that new ID and the pod's current gRPC target.
- The previous process's heartbeat stops, and the existing three-phase
  cleanup handles the old server registration and its jobs according to the
  current lifecycle rules.

Because the new process has a different `serverId`, a request carrying the
old ID must not be accepted by the replacement, even though both processes
use the same StatefulSet DNS name and port at different times.

The requester should resolve the endpoint from the current registration for
the requested `serverId`. If that old registration has been removed or
expired by cleanup, the requester should report that the owning server is no
longer available; it should not silently substitute a different server ID.

## Cancel, delete, and Redis completion semantics

Keep the meanings of the existing operations distinct:

- **Cancel** asks the owning server to stop a running job using the server's
  existing cancellation behavior.
- **Delete** invokes the server's existing job termination/deletion behavior.
  Its exact semantics should remain defined by the existing API.

The server should update Redis through the existing `RedisJobStatus`
lifecycle API when the operation has actually reached the appropriate
terminal state. Do not mark a job complete merely because a cancel/delete RPC
was received or accepted if the job is still running. Preserve the existing
three-phase cleanup behavior for servers that stop heartbeating.

Make cancel/delete handling idempotent where practical, since callers may
retry after a timeout even when the first request was processed.

## Operational considerations

- Ensure the headless Service selector matches the StatefulSet pod labels.
- Ensure the StatefulSet uses that Service as its `serviceName`.
- Ensure the gRPC port is exposed by the container and Service.
- Ensure cluster DNS can resolve the pod-specific fully qualified names from
  every requester that needs to issue cancel/delete calls.
- Configure readiness and headless-Service DNS publication consistently with
  the registration/startup sequence. If clients must discover a pod before it
  becomes Ready, evaluate `publishNotReadyAddresses`; do not enable it
  without considering whether clients could connect before the gRPC server
  is ready.
- Use the appropriate in-cluster authentication and transport security for
  gRPC; DNS discovery is not an authorization mechanism.
- Keep heartbeat expiry and registration cleanup as the authority for whether
  a server registration is current. DNS resolving successfully does not prove
  that the process is alive or owns a job.


## Two-Service pattern: load-balanced creation and direct server routing

The same StatefulSet pods can be exposed through **two Kubernetes Services**.
This does not require two sets of gRPC servers or two listeners. Both Services
select the same pods and expose the same gRPC port, but they serve different
client-routing purposes:

- A normal **ClusterIP Service** gives job-creation clients one stable DNS
  name. Kubernetes routes connections to eligible backend pods.
- A **headless Service** gives each StatefulSet pod an individually
  addressable DNS name. The server advertises its own pod-specific endpoint
  in registration metadata so cancel and delete requests can reach the
  process that owns the job.

For the example namespace and names used in this document:

| Purpose | DNS target |
| --- | --- |
| Create jobs through any eligible replica | `job-server.jobs.svc.cluster.local:50051` |
| Directly reach pod 0 | `job-server-0.job-server-headless.jobs.svc.cluster.local:50051` |
| Directly reach pod 1 | `job-server-1.job-server-headless.jobs.svc.cluster.local:50051` |
| Directly reach pod 2 | `job-server-2.job-server-headless.jobs.svc.cluster.local:50051` |

### Service definitions

Both Services use the same selector, `app: job-server`, and the same port.
The normal Service has a ClusterIP; the headless Service explicitly sets
`clusterIP: None`.

```yaml
# Stable load-balanced entry point for job creation.
apiVersion: v1
kind: Service
metadata:
  name: job-server
  namespace: jobs
spec:
  type: ClusterIP
  selector:
    app: job-server
  ports:
    - name: grpc
      protocol: TCP
      port: 50051
      targetPort: grpc

---
# Per-pod DNS discovery for targeted cancel and delete.
apiVersion: v1
kind: Service
metadata:
  name: job-server-headless
  namespace: jobs
spec:
  clusterIP: None
  selector:
    app: job-server
  ports:
    - name: grpc
      protocol: TCP
      port: 50051
      targetPort: grpc
```

The StatefulSet's `spec.serviceName` must be `job-server-headless`.
Its pod template must carry the matching `app: job-server` label, and the
container must declare a named port `grpc` so the Services' `targetPort:
grpc` resolves correctly.

### StatefulSet pod configuration and environment

The following is the relevant portion of a StatefulSet. It shows the
governing headless Service, three example replicas, the named gRPC container
port, and environment variables supplied to the C++ process.

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: job-server
  namespace: jobs
spec:
  serviceName: job-server-headless
  replicas: 3
  selector:
    matchLabels:
      app: job-server
  template:
    metadata:
      labels:
        app: job-server
    spec:
      containers:
        - name: job-server
          image: your-job-server-image
          ports:
            - name: grpc
              containerPort: 50051
              protocol: TCP
          env:
            # Downward API: values are specific to this pod.
            - name: POD_NAME
              valueFrom:
                fieldRef:
                  fieldPath: metadata.name
            - name: POD_NAMESPACE
              valueFrom:
                fieldRef:
                  fieldPath: metadata.namespace

            # Shared endpoint configuration.
            - name: GRPC_SERVICE_NAME
              value: "job-server-headless"
            - name: GRPC_PORT
              value: "50051"
            - name: K8S_CLUSTER_DOMAIN
              value: "cluster.local"
```

The cluster domain is commonly `cluster.local`, but it is configurable in
Kubernetes. Set `K8S_CLUSTER_DOMAIN` to the actual cluster domain if yours
differs. The C++ process does not need to read or parse YAML; Kubernetes
injects these environment variables into the container.

### Constructing the advertised endpoint in C++

The process uses its pod name, namespace, headless Service name, cluster
domain, and common gRPC port to construct its own endpoint. For example,
`job-server-1` advertises:

```text
job-server-1.job-server-headless.jobs.svc.cluster.local:50051
```

An example helper is:

```cpp
#include <cstdlib>
#include <stdexcept>
#include <string>

std::string getRequiredEnv(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\\0')
    {
        throw std::runtime_error(
            std::string("Missing environment variable: ") + name);
    }

    return value;
}

std::string getGrpcTarget()
{
    const auto podName = getRequiredEnv("POD_NAME");
    const auto podNamespace = getRequiredEnv("POD_NAMESPACE");
    const auto serviceName = getRequiredEnv("GRPC_SERVICE_NAME");
    const auto port = getRequiredEnv("GRPC_PORT");
    const auto clusterDomain = getRequiredEnv("K8S_CLUSTER_DOMAIN");

    return podName + "." + serviceName + "." +
           podNamespace + ".svc." + clusterDomain + ":" + port;
}
```

The gRPC server still listens on all interfaces at
`0.0.0.0:50051` (or the configured `GRPC_PORT`). The pod-specific DNS
name is the advertised address used by other clients; it is not the local
listen address. Every pod can use the same port because each pod has its own
network namespace and IP address.

### Which Service each operation uses

| Operation | Target | Reason |
| --- | --- | --- |
| Create job | `job-server.jobs.svc.cluster.local:50051` | Let Kubernetes select an eligible server replica. |
| Cancel job | The `grpcTarget` registered for the owning `serverId` | Reach the exact server process that owns the job. |
| Delete job | The `grpcTarget` registered for the owning `serverId` | Reach the exact server process that owns the job. |

The job-creation request goes to the normal Service. The selected server
creates the job and its `serverId` remains the owner recorded in the existing
job metadata. For cancel or delete, the requester looks up that owner
`serverId` in the registration metadata and connects to its registered
pod-specific `grpcTarget`. The cancel/delete RPC should carry both
`serverId` and `jobId`; the receiving process validates that the requested
server ID matches its own current process ID and that it owns the job.

**Important gRPC load-balancing detail:** a Kubernetes Service distributes
TCP connections, not individual gRPC RPCs. If a client reuses a long-lived
gRPC channel, many job-creation RPCs can continue to reach the same pod.
If distribution across replicas is important, account for channel reuse and
connection behavior, or use an appropriate gRPC client-side load-balancing
policy and resolver. A single ClusterIP target does not guarantee
round-robin distribution of every RPC.

The two-Service design preserves the existing ownership and cleanup model:
the normal Service is only the entry point for new job-creation connections;
the per-pod endpoint in registration metadata is the route for targeted
operations. DNS is connectivity information, while the globally unique
`serverId` remains the process identity.

## Summary

Use a StatefulSet plus a headless Service to give every pod a predictable,
directly addressable DNS name. Have Kubernetes provide `POD_NAME` and
`POD_NAMESPACE` through environment variables, configure the Service name
and common gRPC port as environment variables, and construct the target in
C++. Continue to use the globally unique per-process `serverId` as the
ownership identity in Redis and in cancel/delete RPCs. This adds Kubernetes
connectivity without changing the fundamental server identity or job
lifecycle model.
