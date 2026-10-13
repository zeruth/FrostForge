# Deliberate stubs

Reference functions that FrostLite has identified, tagged and **chosen not to port yet**, with
what the reference does and why it waits. Ordinary not-yet-reached stubs are not listed here:
every unported function is a tagged `FROST_UNIMPLEMENTED()` body, and a run logs the ones it
reaches. This file is for the ones held back on purpose, or kept null so others can run.

## Network services (user decision, 2026-10-08)

Held back until they are ported against our own server (the AzerothCore evolution project).
None of them may contact Blizzard endpoints.

| function | ref | what the reference does | FrostLite now |
|---|---|---|---|
| `TelemetryInitialize` | 0x141565d60 | registers `telemetryTargetPackage`, makes a session id, starts the telemetry SDK (pkg/telemetry-sdk-cpp, protobuf messages over curl) with four context providers | destroys the providers it was given and does nothing |
| `TelemetryProvideIds/Client/Character/World` | 0x1402281a0, 0x140228220, 0x140227f50, 0x140228060 | fill telemetry context messages (ids, client, character, world position) | stubs, never called |
| `AgentReadInstallConfig` | 0x1415644d0 | reads `.agent.db` (uid, expansion level, data and update dirs, product) into a 0xb0-byte install config | stub; no install config |
| `AgentInitialize` | 0x141563eb0 | opens `Logs\agent.log`, creates the AgentCommunicator (HTTP to the local Battle.net Agent) | stub; no communicator, so every Agent notification is a no-op exactly as the reference's own null checks make it |
| `AgentSend` | 0x141564430 | queues a command for the communicator thread | stub; unreachable without a communicator |
| `AgentShutdown` (instance part) | 0x141565530 | stop handshake (waits up to 1 s), connection teardown | unreachable without a communicator |
| `AgentEventCallback` | 0x140226ea0 -> 0x1402271a0 | handles events Agent sends back | stub, never called |
| `WowDownloadCreate` | 0x141555d80 (ctor 0x141555800) | creates the background CDN downloader | stub; holder left null |
| `WowDownloadStart` | 0x141557640 | sets the downloader's started flag | **diverged**: null-checked while the downloader is a stub |

## ClientServices (login / realm connection)

| function | ref | what the reference does | FrostLite now |
|---|---|---|---|
| `ClientServicesConstruct` | 0x141569c00 | NetClient / RealmConnection base (0x1414f7080, which also installs the realm packet handlers), `Logs/Connection.log`, a handler list | stub returning null: the instance stays null until the net layer is ported |
| `ClientServicesSetStatusCallback` | 0x14156dc90 | stores a callback at instance+0x858 | **diverged**: null-checked |
| `ClientServicesShutdown` (instance part) | 0x14156b7f0 | ends the session (vtable +0x58, reason 0xd) and deletes the instance | unreachable while the instance is null |
| `LogoutCommand` | 0x14156b7c0 | `logout`: ends the session when one is up | stub |
| `ClientServicesStatusCallback` | 0x140227770 | ClientServices' readiness poll | stub, never called |

## Diverged on purpose

| function | ref | what the reference does | FrostLite now |
|---|---|---|---|
| small-block allocator frame step | 0x1412c51b0 (0x1412c4750, 0x1412c49e0) | once a frame, moves the blocks other threads freed back to their size classes in the heap at DAT_1430a8c70; every 600th frame trims it on the thread pool | left out of the frame (both calls in FUN_140225ab0): Storm's allocations go through the CRT, and the hooks that set the heap up (FUN_1412c46b0) are a stub, so there is no heap to keep |

## Local, not yet ported

| function | ref | why it waits |
|---|---|---|
| `RegionDialogRun` | 0x140219cc0 (dlgproc 0x14021a7d0) | the region/language picker is a dialog resource (0x90) with string resources in the reference exe; FrostLite has no resources yet. Only shown when the portal or locale had to be guessed (a fresh install). The stub returns **cancel**, so on a fresh install startup stops there - set `portal` and `textLocale` in WTF\Config.wtf. |
| `OsRegistryReadString` | 0x141dfb910 | reads `HKCU\Software\Blizzard Entertainment\Battle.net\Launch Options\WoW` values (DPAPI-decrypted when asked); only reached with `-launcherlogin` |
| `LibraryAddRef` / `LibraryRelease` | 0x142204650 / 0x142204520 | a third-party library's ref-counted global init/shutdown; only on the startup cancel path; not identified |
| Locale CVar callbacks | 0x140226bd0, 0x140226d90, 0x140226460, 0x140226a10 | reach the locale/portal code they share with StartupService; they accept every value for now |
| Timing, movie log | 0x141237d90, 0x1405fde10 | stage 2 edges, next in line |
