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
| `AgentEventCallback` | 0x140226ea0 -> 0x1402271a0 | the callback installed in Agent (DAT_1430d2548); the world connection's AUTH_CHALLENGE handler (FUN_141232760) calls it through FUN_1415679c0 to fill the 16-byte Win64 auth seed | ported (2026-10-20): forwards to the translated FUN_1402271a0. Was a stub, which left the seed unset |
| `WowDownloadCreate` | 0x141555d80 (ctor 0x141555800) | creates the background CDN downloader | stub; holder left null |
| `WowDownloadStart` | 0x141557640 | sets the downloader's started flag | **diverged**: null-checked while the downloader is a stub |

## ClientServices (login / realm connection)

| function | ref | what the reference does | FrostLite now |
|---|---|---|---|
| `ClientServices` (class) | 0x141569c00, 0x14156b1d0; RealmConnection 0x1414f7080, 0x1414f73c0; NetClient 0x141231cc0, 0x1412320d0 | the session object, its realm-message handlers, `Logs/Connection.log`, the event queue (NetInternal) | ported by hand (2026-10-13). Its virtuals that are not ported yet (the Handle* slots, Poll, the WowConnection callbacks) and the WowConnection, PacketPipe and JAM message code under it run translated |
| `ClientServicesSetStatusCallback` | 0x14156dc90 | stores a callback at instance+0x858 | ported, as the reference (no null check) |
| `ClientServicesShutdown` | 0x14156b7f0 | ends the session (reason 0xd) and deletes the instance | ported; reached from the stage 2 teardown (FUN_140227810, ported) and the startup cancel path |
| `TelemetryShutdown` | 0x141565c10 | stops the telemetry service (waits up to 5 s), releases its objects, shuts curl down | ported as far as its flag: offline the service never started, so the body is a stub |
| `WowDownloadDestroy` | 0x141556340 | destroys the downloader (0x4f8 bytes) | ported as far as its null check: offline there is no downloader |
| `BattleNetShutdown` | 0x141487dc0 | logs "Shutting down", destroys the Aurora and login services, clears two realm-connection callbacks | stub, like `BattleNetInitialize` (Phase 2-3) |
| `LogoutCommand` | 0x14156b7c0 | `logout`: ends the session when one is up | stub |
| `ClientServicesStatusCallback` | 0x140227770 | ClientServices' readiness poll, asked before a received character list goes to the glue | ported (2026-10-20). Was a stub answering false, so the list never reached CharacterSelect |
| `Ed25519VerifyCore` (src/net/Ed25519Verify.cpp) | 0x1420bf4f0 | the Ed25519ctx check behind SMSG_ENTER_ENCRYPTED_MODE: hashes dom2 \|\| R \|\| A \|\| M, computes [S]B - [k]A through the curve's own operations and compares it with R | ported by hand (2026-10-20), because the decompilation dropped the 32-byte copy of the result's x (Ghidra typed its source as a 4-byte local), so the translation refused every signature (disconnect reason 24). Its callees stay translated, and the curve operations run native |

## Diverged on purpose

| function | ref | what the reference does | FrostLite now |
|---|---|---|---|
| startup working directory (stage 1) | 0x1402262e0 (0x140211520) | with no `-datadir`, moves to its executable's directory, the install it ships in | keeps the directory it was started in: FrostLite's exe lives in the build tree. Exposed 2026-10-18 when `OsPathStripFilename` stopped being a stub and the move began to succeed (no Config.wtf, no .build.info, CAS init failed) |
| the anti-tamper layer's routines game code calls | 0x14021c040 (frame tick), 0x1400b1420 (key stub), 0x14029fc80 (code hash), 0x1402a5bc0 (timing check), 0x1402bd260 / 0x1402bd150 (Lua taint shadow set / check) | the protector's runtime: a per-frame tick over its context, the key schedule its masked values decode through, a crc32 over code pages, rdtsc timing under its spinlock (tools/aegis.py) | **diverged**: no protector runs, so each does what the reference's does when the protection is satisfied and nothing else: the tick and the timing check do nothing, the key is left as it is (values encoded and decoded through it stay consistent), the hash passes, the taint shadow is neither kept nor checked (the port's Lua has none) (`Sub14021C040` in StartupClient.cpp, the rest in src/client/Protection.cpp, 2026-10-19) |
| world server key (`PacketPipeInstallServerKey`, src/net/WowConnection.cpp) | 0x1427b4358, read by 0x141561000 | verifies SMSG_ENTER_ENCRYPTED_MODE's Ed25519ctx signature (context 0x1427b4348) against Blizzard's public key compiled in | **diverged** (D4, 2026-10-20): the 32 bytes are replaced with FrostForge's public key once the reference image is mapped; algorithm, context and signed message unchanged. The private key is the worldserver's `worldserver-ed25519.key.pem` |
| small-block allocator frame step | 0x1412c51b0 (0x1412c4750, 0x1412c49e0) | once a frame, moves the blocks other threads freed back to their size classes in the heap at DAT_1430a8c70; every 600th frame trims it on the thread pool | left out of the frame (both calls in FUN_140225ab0): Storm's allocations go through the CRT, and the hooks that set the heap up (FUN_1412c46b0) are a stub, so there is no heap to keep |
| FFX effect configure (`FfxEffectBegin`, src/ui/CSimpleModelFFX.cpp) | 0x1405dd5d0 | calls the slot's effect (vtable +0x28) without checking it: the FFX module fills every slot at startup | **diverged** (2026-10-21): the FFX module is not ported, so its slots stay null; an empty slot is skipped (its translation faulted when CharacterSelect selected a character) |

## Local, not yet ported

| function | ref | why it waits |
|---|---|---|
| `OsRegistryReadString` | 0x141dfb910 | reads `HKCU\Software\Blizzard Entertainment\Battle.net\Launch Options\WoW` values (DPAPI-decrypted when asked); only reached with `-launcherlogin` |
| `LibraryAddRef` / `LibraryRelease` | 0x142204650 / 0x142204520 | a third-party library's ref-counted global init/shutdown; only on the startup cancel path; not identified |
| Locale CVar callbacks | 0x140226bd0, 0x140226d90, 0x140226460, 0x140226a10 | reach the locale/portal code they share with StartupService; they accept every value for now |
| The teardown after the save | 0x14022a040, 0x14123abd0, 0x1402293f0, 0x1402285e0, 0x140323870 (console commands), 0x14031fad0 (`CVarShutdown`) | stage 4, the window system and stage 3 torn down, then the console commands and every CVar freed. The final shutdown (FUN_140216880) and the stage 2 teardown (FUN_140226550) are ported and write Config.wtf, but these stay stubs: the sound engine, the UI and the tact workers are never stopped, and still read CVars and commands until the process ends |
| `GamePadSaveAddonConfig` | 0x14152ad60 | writes WTF\GamePadConfig_Addons.json on quit | no game pad support is ported |
