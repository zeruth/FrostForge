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
| D3 | FrostForge is converted to 3.4.3 **on every level**, and 3.3.5 support is removed entirely. That covers the authentication (Battle.net in place of authserver's SRP6), the world protocol, the opcodes and update fields, client data (3.4.3 DB2s in place of 3.3.5 DBCs), the database schema and the game rules. Nothing is translated or kept for 3.3.5 compatibility. | **made** (2026-10-13) |
| D4 | FrostLite connects to FrostForge on **127.0.0.1**, with the **absolute minimum** of client changes. Every change is a recorded `diverged` entry in `docs/stubs.md`, with the reference's behaviour beside it. The reference resolves `<portal>.actual.battle.net` (from the portal CVar, e.g. "US") and connects with Windows SChannel TLS, which validates the server's certificate. Phase 2 reads exactly how the host is built and the certificate checked, then picks the smallest change that reaches 127.0.0.1 with FrostForge's certificate. | **made**; the exact change is chosen in Phase 2 |
| D5 | Characters are stored the 3.4.3 way: customizations as ChrCustomizationChoice ids in FrostForge's schema, in place of 3.3.5's skin/face/hair/facial-hair bytes. No mapping layer. | **made** (2026-10-13), follows from D3 |

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
- [x] Hand-port the constructor and its base: `ClientServices` (0x141569c00, dtor 0x14156b1d0),
      `RealmConnection` (0x1414f7080, dtor 0x1414f73c0, the realm-message handlers and callback
      setters 0x1414f8790..0x1414f8970) and `NetClient` (0x141231cc0, dtor 0x1412320d0) are C++
      classes in `src/net` and `src/services`, laid out as the reference (static_asserts) with its
      vtable slot order. Slots not ported yet forward to translated code. The glue response object
      (DAT_142d098e8) is ported too: the instance was built with null there before.
- [x] NetClient.cpp: 0x141233a70, 0x141233c00, 0x141234230, 0x141234c30, 0x141234d00, 0x141235dd0
      (plus 0x141233840, 0x141233de0, 0x141233e40, 0x141234f90, 0x1412325a0, 0x1412348c0..0x1412349f0).
- [x] NetInternal: 0x141235f60, 0x141236120, 0x141236220, 0x141236300 (plus the queue's
      constructor, destructor and clear, 0x141235eb0, 0x141235f00, 0x1412361d0).
- [x] Undo the null-check divergences: `ClientServicesSetStatusCallback` without the check, and
      `ClientServicesShutdown`'s instance part ported (end the session, delete the instance).
- [x] Connection.log reaches disk. The teardown that closes it is stage 2's (FUN_140227810, which
      calls ClientServicesShutdown), ported with stage 4's (FUN_1402293f0). Note: the reference's
      name is `Logs/Connection.log` with a forward slash, and Storm resolves a name without a
      backslash against the executable's folder, so FrostLite writes it to
      `build/cmake/bin/Release/Logs/`, not `client/_classic_/Logs/` (faithful; the reference's exe
      sits in `_classic_`).
- Open: the server key's big numbers (FUN_1420cd4e0) are a demoted stub, so they leak at exit.
- **Check:** the client starts with a live ClientServices instance and the login screen still works.
  **Seen 2026-10-13** with the hand port: login screen, then a clean close (WM_CLOSE, exit 0), and
  Connection.log holding "Connection Initialize" and "Connection Destroy".

### Phase 2: Battle.net transport

The client side of the Battle.net (bgs) connection: TLS, RPC framing and the protobuf runtime.

- [x] **First:** how the reference builds the Battle.net host and checks the server's certificate.
      Read 2026-10-13. **Result: no client change is needed (D4 met with zero divergences).**
      - Host: `PortalGetHost` (0x141566cb0) appends `.actual.battle.net` only to a portal that is
        not an address and has no dot, and splits `host:port` (default 1119). So
        `SET portal "127.0.0.1"` (or `"127.0.0.1:<port>"`) in `WTF/Config.wtf` points it at us.
      - TLS: bgs-sdk's cross_ssl layer creates one global factory (0x141f2ece0) with type 0, which
        maps to SChannel (0x141f73c30), **no trust certificates** (settings +0x30 null) and **the
        system store allowed** (+0x38 = 1). SChannel credentials are acquired with
        `SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS` (0x141f71c80): SChannel
        checks nothing, the client's own verifier does (0x141f707f0 -> 0x141f769d0).
      - Chain check (0x141f769d0): `CertGetCertificateChain` on the current-user engine, then the
        local-machine one; EKU serverAuth (or the two SGC usages); no revocation check; time
        validity checked; `CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL)` with no server
        name. No certificate or key is pinned (the only PEM markers in the binary are curl's).
      - Host check (network_connection.cpp 0x141f33ec0 -> 0x141f34250): the certificate must have
        a common name; each subject-alternative name is matched against the remote host string,
        and failing that the common name against the connected endpoint's address.
      - So FrostForge's certificate needs: CN=127.0.0.1, SAN IP:127.0.0.1 (and DNS:localhost),
        EKU serverAuth, issued by a local FrostForge root CA added to the current user's Trusted
        Root store (no admin needed). The login REST endpoint (Phase 3) goes through curl's own
        schannel backend, which has its own pinning option: read it in Phase 3.
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
- **Server:** the login endpoint (REST) checking the account
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
- Nothing contacts Blizzard (D1). Ports and hosts stay local while developing.
- Each phase is one or more scoped commits. Record the commits in the progress log.

## Progress log

| date | phase | what landed | commits |
|---|---|---|---|
| 2026-10-13 | - | roadmap written; FrostForge confirmed 3.3.5-only; the reference's TLS is SChannel | 4b79686 |
| 2026-10-13 | 1 | the session is live (translated constructor); null-check divergences removed; ClientServicesShutdown's instance part ported | see git log |
| 2026-10-13 | - | decisions made: D3 (FrostForge goes 3.4.3 on every level, 3.3.5 removed), D4 (127.0.0.1, minimal recorded client changes), D5 (native customization schema) | this commit |
| 2026-10-13 | 1 | ClientServices / RealmConnection / NetClient / NetInternal by hand (src/net); stage 2 and 4 teardowns; Connection.log written; phase 1 done | this commit |
| 2026-10-13 | 2 | the host and certificate read: no client change needed; FrostForge needs a cert (CN/SAN 127.0.0.1, serverAuth) from a local root in the user's Trusted Root store | this commit |
