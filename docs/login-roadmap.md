# Roadmap: login to character select

The goal: type an account and password on FrostLite's login screen, log in against our own server,
see the realm list, connect, and land on the character select screen with the characters drawn.
Character creation, deletion and entering the world come after this roadmap.

The server is **azerothcore-FrostForge** (`C:\Users\tyler\Documents\code\azerothcore-FrostForge`,
`github.com/zeruth/azerothcore-FrostForge`), a fork of azerothcore-wotlk.

**Keep this file current.** Tick items as they land, add the commit, and add a line to the progress
log at the bottom. A future session should be able to start from this file alone.

## The core problem

The two ends speak different protocols:

| | FrostLite (3.4.3.54261) | FrostForge today (3.3.5a, build 12340) |
|---|---|---|
| login | Battle.net: protobuf RPC over TLS (port 1119), a login ticket from a web/REST login | authserver: SRP6 over a plain socket (port 3724) |
| realm list | GameUtilities RPC, zlib-compressed JSON | authserver realm list packet |
| world | world socket with the modern auth, encryption and packet framing; 3.4.3 opcodes and update fields | 3.3.5 world socket: RC4 header encryption, 3.3.5 opcodes |
| characters | customizations as ChrCustomizationChoice ids | skin/face/hair/facial-hair bytes |

FrostForge is becoming a **3.4.3 server on every level** (D3): protocol, data, database schema and
game rules. 3.3.5 support is removed, not kept beside it. This roadmap is the first stretch of that
conversion: the parts a client needs to reach character select. The **client track** ports
FrostLite's network stack, offline until now by design. The two tracks meet phase by phase.

## Decisions

| # | Decision | State |
|---|---|---|
| D1 | No Blizzard endpoints, ever. The client only talks to our server. | **made** (2026-10-08) |
| D2 | FrostLite stays a 1:1 port. Anything we must change to reach our server is recorded as `diverged` in `docs/stubs.md`. | **made** |
| D3 | FrostForge is converted to 3.4.3 **on every level**, and 3.3.5 support is removed entirely. That covers the authentication (Battle.net in place of authserver's SRP6), the world protocol, the opcodes and update fields, client data (3.4.3 DB2s in place of 3.3.5 DBCs), the database schema and the game rules. Nothing is translated or kept for 3.3.5 compatibility. TrinityCore's `wotlk_classic` branch is a source to port from; FrostForge's AGENTS.md requires `--author` credit for anything mirrored from it. | **made** (2026-10-13) |
| D4 | FrostLite connects to FrostForge on **127.0.0.1**, with the **absolute minimum** of client changes. Every change is a recorded `diverged` entry in `docs/stubs.md`, with the reference's behaviour beside it. The reference resolves `<portal>.actual.battle.net` (from the portal CVar, e.g. "US") and connects with Windows SChannel TLS, which validates the server's certificate. Phase 2 reads exactly how the host is built and the certificate checked, then picks the smallest change that reaches 127.0.0.1 with FrostForge's certificate. | **made**; the exact change is chosen in Phase 2 |
| D5 | Characters are stored the 3.4.3 way: customizations as ChrCustomizationChoice ids in FrostForge's schema (as TrinityCore keeps them), in place of 3.3.5's skin/face/hair/facial-hair bytes. No mapping layer. | **made** (2026-10-13), follows from D3 |

## Where things stand (2026-10-13)

- Login screen: done. The scene, music, model sounds, keyboard, credits and cinematics all work,
  and settings persist through Config.wtf.
- Clicking **Login** stops in `FUN_1416a0db0` (the Battle.net login), a deliberate stub.
- `ClientServicesConstruct` (0x141569c00) returns null, so no session object ever exists. Telemetry,
  Agent and WowDownload are offline stubs (`docs/stubs.md`); they stay that way for this roadmap.
- FrostForge: stock AzerothCore, 3.3.5 only.

## Phases

Each phase has client items, server items, and a check that proves it works. A phase is not done
until its check has been seen working, with both programs running locally.

### Phase 0: local setup

Waived by the user (2026-10-13): no separate baseline step. FrostLite builds and runs in CLion, and
FrostForge is brought up when the first server item needs it. The items below are done as they
come up, not as a gate.

- [ ] ~~FrostForge builds and runs locally as it is (3.3.5): the baseline before the conversion.~~
      Skipped.
- [ ] Plan the conversion's order with FrostForge's own planning docs (`.agents/plans/`, per its
      AGENTS.md). This roadmap covers only what character select needs. The rest of the conversion
      (maps, spells, movement, everything in world) gets its own plan.
- [ ] Server data: extract the 3.4.3 client data FrostForge will load (DB2s first; maps and vmaps
      later) from the FrostLite install.

### Server groundwork (D3)

The 3.4.3 conversion pieces that every later phase stands on. They can proceed alongside Phases 1-2.

- [ ] Client data: replace the DBC stores with 3.4.3 DB2 loading, starting with what login and
      character select read: ChrRaces, ChrClasses, ChrCustomization*, ChrModel, CharBaseInfo,
      AreaTable/Map (for the zone shown per character), Item displays.
- [ ] Auth database: Battle.net accounts (email-style login) holding the game accounts, replacing
      authserver's SRP6 account table.
- [ ] Characters database: the 3.4.3 character schema, with customizations as ChrCustomizationChoice
      rows (D5).
- [ ] Remove authserver (the 3.3.5 SRP6 login service); Phase 2-4's Battle.net service replaces it.

### Phase 1: the session object (client)

The client's network core, so a session exists for the login code to drive.

- [x] The session exists: `ClientServicesConstruct` 0x141569c00 runs translated and returns a live
      instance with the reference's vtable (0x1427b5518), seen in a run on 2026-10-13. The NetClient /
      RealmConnection base 0x1414f7080 runs inside it.
- [ ] Hand-port the constructor and its base 0x1414f7080 (the realm packet handlers, Connection.log)
      from translated to hand code.
- [ ] NetClient.cpp: 0x141233a70, 0x141233c00, 0x141234230, 0x141234c30, 0x141234d00, 0x141235dd0.
- [ ] NetInternal: 0x141235f60, 0x141236120, 0x141236220, 0x141236300.
- [x] Undo the null-check divergences: `ClientServicesSetStatusCallback` without the check, and
      `ClientServicesShutdown`'s instance part ported (end the session, delete the instance).
- [ ] Connection.log reaches disk: it is written in memory, but logs flush only when closed, and the
      stage 3 teardown that closes it (FUN_1402293f0) is a stub.
- **Check:** the client starts with a live ClientServices instance and the login screen still works.
  **Seen 2026-10-13** (the user's run: login screen, Login reached FUN_1416a0db0, a clean close).

### Phase 2: Battle.net transport

The client side of the Battle.net (bgs) connection: TLS, RPC framing and the protobuf runtime.

- [ ] **First:** read how the reference builds the Battle.net host from `portal` and how it validates
      the server certificate (SChannel credential setup, the certificate callback, any pinning). Then
      make D4's minimal change: just enough to reach 127.0.0.1 and accept FrostForge's certificate,
      recorded as `diverged` in `docs/stubs.md`. Write the change and the Config.wtf lines it needs here.
- [ ] The TLS transport over SChannel (the reference's strings: "SChannel GetClientHello failed",
      "SChannel decrypt failed"), and the portal host resolution (`<portal>.actual.battle.net`,
      `.bgs.battle.net`).
- [ ] rpc_connection.cpp (0x141f02560 to 0x141f069a0) and rpc_connection_manager.cpp
      (0x141f08070 to 0x141f09460): framing, request/response matching, service binding.
- [ ] The protobuf runtime (about 770 tagged functions) and the generated messages for the
      services we need. Most of this should go through the translator. The `club_*` messages and
      other unneeded services can stay untranslated.
- **Server:** a Battle.net listener on 1119 (TLS) with the RPC framing, and the Connection
  service (Connect, Bind).
- **Check:** the client completes the TLS handshake and Connection.Connect with FrostForge.
  Both logs show it.

### Phase 3: logging in

- [ ] GlueLogin.cpp: 0x14169cd70 and 0x14169d0c0 (translated today); 0x14169e560 to 0x14169fb00;
      0x14169ffd0; and **0x1416a0db0**, what the Login button calls.
- [ ] The Authentication service client: authentication_service_outbound_cb.cpp 0x141dfc1e0,
      0x141dfc5d0, 0x141dfc780 (translated today). Logon, then the logon result.
- [ ] How the password reaches the server: the reference sends the credentials to a login endpoint
      and receives a login ticket, then logs on with the ticket. Document the exact exchange here
      once it is read out of the reference.
- [ ] `OsRegistryReadString` 0x141dfb910, only for `-launcherlogin`. It can stay a stub.
- **Server:** the login endpoint (REST, as TrinityCore's bnetserver does it) checking the account
  against the auth database and issuing the ticket; the Authentication service accepting it. Accounts
  may need a Battle.net-style record beside the game account.
- **Check:** a correct password gets past the "Connecting" / "Authenticating" dialogs, and a wrong
  one shows the reference's error.

### Phase 4: realm list and realm join

- [ ] The GameUtilities client: game_utilities_service_outbound_cb.cpp 0x141dfcd50, 0x141dfd130
      (translated today). The realm list request, subscription, and realm join.
- [ ] RealmList.cpp: 0x141568830, 0x141568c10. The JSON (zlib-compressed) into the realm list the
      glue shows.
- [ ] ClientServices.cpp 0x14156d290: realm selection and join.
- **Server:** the realm list as GameUtilities replies (JSON built from AzerothCore's `realmlist`
  table), and the join reply: the world address plus a join ticket and the session key shared with
  the world server.
- **Check:** the realm list shows FrostForge's realm; choosing it starts the world connection.

### Phase 5: the world connection

- [ ] WowConnection.cpp: 0x1414fdb30, 0x1414fdef0 (0x1415002e0 is already ported).
- [ ] PacketPipe: 0x141235030, 0x141235480, 0x1412357c0 (the last two translated today),
      0x1414fefd0, 0x141561390, 0x141561940, 0x141567c60, 0x142577340, 0x14257df20, 0x14257f5b0,
      0x14257fcc0, 0x14257fd10; PacketPipe.cpp 0x141502950; PacketPipe_CliSvr.cpp 0x141561260.
- [ ] WDataStore.cpp: 0x141502410, 0x141502660 (packet reading and writing).
- [ ] The modern world handshake: auth challenge, auth session (the join ticket and session key from
      Phase 4), entering encrypted mode, compression. Write the exact packet layouts into this file
      as they are read out of the reference.
- **Server:** FrostForge's world socket replaced by the 3.4.3 one: the modern framing, encryption
  and opcode table (the 3.3.5 opcodes and handlers go), an auth session checked against the Phase 4
  session key, and the auth response.
- **Check:** the client authenticates to the world server and receives the auth response.

### Phase 6: character list

- [ ] Character enumeration into the C_Login state machine (already ported) and the glue's
      CharacterSelect screen (CharacterSelection.cpp 0x1416943f0).
- [ ] The CharacterSelect Lua API the GlueXML calls (character count, info per character,
      selection). Check `gen_script_api.py`'s stubs for which are missing.
- **Server:** the 3.4.3 enum-characters result, built from the 3.4.3 character schema (customization
  choices stored natively, D5).
- **Check:** the character select screen lists the account's characters by name, level, class and zone.

### Phase 7: the character select scene

- [ ] Each character drawn as a customized player model: ChrModel, ChrCustomization* DB2s,
      CharComponentTextureLayouts/Sections, hair and facial-hair geosets, and the equipped items' displays.
- [ ] CharacterCreation.cpp 0x141686ca0, 0x141687040: shared customization code (also the start of
      character creation).
- **Check:** the selected character stands in the scene and looks as it does in game.

## Rules for this work

- Port from the reference, as everywhere in FrostLite: `// ref:` tags, the reference's structures
  and behaviour. Do not write client protocol code from a server's point of view.
- Server code mirrored from TrinityCore is committed with `--author` naming the original author
  (FrostForge AGENTS.md).
- Nothing contacts Blizzard (D1). Ports and hosts stay local while developing.
- Each phase is one or more scoped commits. Record the commits in the progress log.

## Progress log

| date | phase | what landed | commits |
|---|---|---|---|
| 2026-10-13 | - | roadmap written; FrostForge confirmed 3.3.5-only; the reference's TLS is SChannel | 4b79686 |
| 2026-10-13 | 1 | the session is live (translated constructor); null-check divergences removed; ClientServicesShutdown's instance part ported | see git log |
| 2026-10-13 | - | decisions made: D3 (FrostForge goes 3.4.3 on every level, 3.3.5 removed), D4 (127.0.0.1, minimal recorded client changes), D5 (native customization schema) | this commit |
