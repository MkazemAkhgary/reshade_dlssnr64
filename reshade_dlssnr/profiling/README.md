# Optick profiling policy

Optick scopes are emitted through `profiling/profile.h`. The MSBuild properties
below become preprocessor constants, so disabled verbosity levels and domains
compile to no-ops instead of leaving dormant per-frame instrumentation branches.

| Level | Intended coverage | Cost |
|---:|---|---|
| 0 | No addon scopes or frame markers | Lowest; Optick source remains linked for the optional GPU integration surface |
| 1 | Major callbacks, frame boundaries, pass-stage entry, UI overlay entry | Low |
| 2 | Individual pipeline steps, allocations, queue/fence operations, readback and async workers, per-dispatch CPU scopes | Moderate |
| 3 | Deep helper paths, individual schema/settings rows, detailed validation and nested UI/ctl operations | Highest; use for short diagnostic captures |

Domains are `CORE` (addon lifecycle and callbacks), `RENDER` (pass/pipeline/compute CPU work),
`RESOURCE` (resource creation, state, retirement and lifetime), `ASYNC` (workers, queue/fence,
readback and deferred work), `UI` (overlay and interactions), `CTL` (schema, value store and
persistence), and `GPU` (D3D12 timestamp events). GPU profiling is disabled by default; enabling
it requires a compatible D3D12 adapter/queue and has a higher measurement cost.

Defaults are verbosity 1, with CPU domains enabled and GPU disabled. Example full diagnostic
build with all nested zones and GPU events enabled:

```powershell
msbuild reshade_dlssnr.sln /p:DlssnrProfileVerbosity=3 /p:DlssnrProfileDomainGpu=1
```

Lower-overhead focused captures can disable unused domains, for example
`/p:DlssnrProfileDomainUi=0 /p:DlssnrProfileDomainCtl=0`, or turn all addon zones off with
`/p:DlssnrProfileVerbosity=0`. Existing code should use these wrappers rather than raw
`OPTICK_EVENT` or `OPTICK_TAG` calls so the project-wide switches remain effective.
