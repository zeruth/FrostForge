# SMSG_UPDATE_OBJECT (0x27CB) in 3.4.3.54261

Everything here was read out of the client's own parser (`client/_classic_/WowClassic-clean.exe`);
the function addresses are the evidence. Field *names* follow TrinityCore's 3.4.x
(`wotlk_classic`) code where the client's shape matches it; a name the client alone cannot
confirm is marked **[inferred]**. Widths, order, bit positions and gating are all from the
client.

The update-field contents (the `ValuesCreate` / `ValuesUpdate` payloads) are in
[updatefields.md](updatefields.md) / [updatefields.json](updatefields.json), extracted by
`tools/updatefields.py`.

## Conventions

* Little-endian. `u8/u16/u32/u64`, `float` = IEEE single.
* **Bits** are read MSB first: the first bit of a byte is `0x80`. A run of bits is followed by
  byte-aligned data only after the partially used byte is abandoned ("FlushBits"): every
  bit-group below is shown with the byte it sits in. Where a structure reads its flags with
  plain `u8` reads (`byte >> 7`, `(byte & 0x40) >> 6`, ...), this is byte-for-byte the same as
  TrinityCore's `WriteBit()... FlushBits()`.
* **PackedGuid** (`0x141550b20`): `u8 lowMask, u8 highMask`, then the non-zero bytes of the low
  64-bit half (LSB first, one per set bit of lowMask), then those of the high half. This is the
  128-bit `ObjectGuid` packed form (`ObjectGuid::WriteAsPacked` in TC).
* **Bit reader** (`0x14071f930`, `(reader, n) -> n bits MSB first`): `{stream*, u8 cur, u8
  pos}`; `pos = 8` means "fetch a byte next". Readers start fresh (`pos = 8`) wherever the code
  re-initializes it, which is where TC calls `FlushBits()`.

## 1. Message header — parser `0x140706c80`, handler `0x1418efa20` -> `0x1418efa70`

| # | type | field | notes |
|---|---|---|---|
| 1 | u32 | NumObjUpdates | number of update blocks in `Data` |
| 2 | u16 | MapID | must equal the current map (`*(0x143189140)+0x160`), else the whole message is dropped with *"Received mismatched map ID (%d while on %d) in object update message -- ignoring"* |
| 3 | byte | bit7 HasRemovedObjects | other 7 bits unused |
| 3a | u16 | DestroyCount | only if HasRemovedObjects |
| 3b | u32 | RemovedCount | only if HasRemovedObjects |
| 3c | PackedGuid × RemovedCount | | only if HasRemovedObjects. The first `DestroyCount` are **destroyed**, the rest go **out of range** (`0x1418effb0`: vtable `+0x1a8(obj, ..., index < DestroyCount)`). The active player's own GUID is skipped. |
| 4 | u32 | DataSize | |
| 5 | bytes[DataSize] | Data | `NumObjUpdates` blocks, below |

There is no "out of range" update *type* in 3.4.3: removals travel in the header.

The handler parses `Data` twice. Pass 1 (`0x1418ef790`) creates/updates objects and records the
stream offset at the end of each block; pass 2 (`0x1418efc60`) re-reads the leading
`type/guid/(objtype + movement)` of each block for the post-create callbacks and then jumps to the
recorded end offset. A block that fails in pass 1 aborts the rest of the message.

## 2. Update blocks (`Data`)

Each block starts with `u8 UpdateType`; anything other than 0, 1, 2 aborts the message (pass 1
logs the remaining bytes with `0x141554b60`).

| UpdateType | name | layout |
|---|---|---|
| 0 | Values | `PackedGuid`, then **ValuesUpdate** (§4.2) via the object's vtable `+0x370` |
| 1 | CreateObject1 | `PackedGuid`, `u8 ObjectTypeID`, **CreateObject movement block** (§3), **ValuesCreate** (§4.1) |
| 2 | CreateObject2 | same as 1. The only difference the client makes is the `bool` passed to the post-create hook (vtable `+0x180(obj, ctx, movement, type == 2)`); TC sends 2 for objects that were just spawned and 1 for objects coming into view. |

**Values on an unknown GUID** (pass 1, `0x1418ef790`): the client logs it, reads `u32 size` and
skips `size` bytes — a server bug there is survivable, a bad create is not.

### ObjectTypeID (create switch in `0x1418eeb80`, max 13)

| id | type | class vtable | constructor |
|---|---|---|---|
| 1 | Item | 0x1427fc2d8 | 0x1417f6880 |
| 2 | Container | 0x1427fe3d8 | 0x14182aa90 |
| 3 | AzeriteEmpoweredItem | — | (no case: not creatable in this build) |
| 4 | AzeriteItem | — | (no case) |
| 5 | Unit | 0x1427f9e70 | 0x14177d490 |
| 6 | Player | 0x1427ef7f8 | 0x14172f5a0 |
| 7 | ActivePlayer | 0x1427ed6d0 | 0x141707290 (Player ctor + ActivePlayerData) |
| 8 | GameObject | 0x1427f4748 | 0x14174e510 |
| 9 | DynamicObject | 0x1427fbdf0 | 0x1417e5100 |
| 10 | Corpse | 0x1427fb7a0 | 0x1417e0df0 |
| 11 | AreaTrigger | 0x1427fb298 | 0x1417d9fe0 |
| 12 | SceneObject | 0x1427fd940 | 0x14181ae70 |
| 13 | Conversation | 0x1427f04d0 | 0x14174bca0 |

0 (Object) is accepted by the range check but has no case: it would create a bare object.
**The player's own character must be created with type 7 (ActivePlayer)** and the `ThisIsYou`
bit (§3) — that is what sets the active-player GUID (`*(0x143189140)+0x150`) and ends
*"Stop waiting for active player"*.

If the GUID already exists, a create re-reads the movement block and the values into the
existing object (vtable `+0x368(obj, stream, 0)`); a new object gets `+0x368(obj, stream, 0x20)`
(`0x20` = "new object, no change callbacks"; it is not a wire flag).

## 3. CreateObject movement block — `0x14070d910`

Three bytes of flags ("CreateObjectBits"), MSB first:

| byte | bit | flag | stored at (+movement struct) | gates |
|---|---|---|---|---|
| 0 | 7 | NoBirthAnim | +0x0 | — |
| 0 | 6 | EnablePortals | +0x1 | — |
| 0 | 5 | PlayHoverAnim | +0x2 | — |
| 0 | 4 | MovementUpdate | +0x3b0 | §3.1 |
| 0 | 3 | MovementTransport | +0x3f0 | §3.3 |
| 0 | 2 | Stationary | +0x408 | 4 floats |
| 0 | 1 | CombatVictim | +0x420 | PackedGuid |
| 0 | 0 | ServerTime | +0x42c | u32 |
| 1 | 7 | Vehicle | +0x438 | u32 + float |
| 1 | 6 | AnimKit | +0x442 | 3 × u16 |
| 1 | 5 | Rotation | +0x450 | u64 |
| 1 | 4 | AreaTrigger | +0x748 | §3.5 |
| 1 | 3 | GameObject | +0x75c | §3.6 |
| 1 | 2 | SmoothPhasing | +0x780 | §3.7 |
| 1 | 1 | ThisIsYou | +0x788 | — (marks the active player) |
| 1 | 0 | SceneObject | +0x960 | §3.8 |
| 2 | 7 | ActivePlayer | +0xc88 | §3.9 |
| 2 | 6 | Conversation | +0xc98 | §3.10 |
| 2 | 5..0 | (unused) | | |

Then, in this exact order:

| # | condition | read | field |
|---|---|---|---|
| 1 | MovementUpdate | §3.1 (`0x14070d420`) | MovementUpdate block |
| 2 | always | u32 | PauseTimesCount (`0x14078f3c0` resizes the array at +0xca0) |
| 3 | Stationary | float ×4 | Stationary X, Y, Z, Orientation (+0x3f8..+0x404) |
| 4 | CombatVictim | PackedGuid | CombatVictim (+0x410) |
| 5 | ServerTime | u32 | ServerTime (GameTime) (+0x428) |
| 6 | Vehicle | u32 | VehicleRecID (+0x430) |
| 7 | Vehicle | float | InitialRawFacing (+0x434); must be finite |
| 8 | AnimKit | u16 ×3 | AiID, MovementID, MeleeID (+0x43c..) [names inferred] |
| 9 | Rotation | u64 | packed local rotation (+0x448) [TC: `GetPackedLocalRotation`] |
| 10 | always | u32 × PauseTimesCount | PauseTimes |
| 11 | MovementTransport | §3.3 (`0x14070d2d0`) | TransportLocation |
| 12 | AreaTrigger | §3.5 (`0x14070a1b0`) | |
| 13 | GameObject | §3.6 | |
| 14 | SmoothPhasing | §3.7 | |
| 15 | SceneObject | §3.8 | |
| 16 | ActivePlayer | §3.9 | |
| 17 | Conversation | §3.10 | |

The values follow immediately (§4.1).

**Validation** (`0x14076c260`, a create is rejected — and the whole message with it — when this
fails): every float must be finite; MovementUpdate excludes MovementTransport **and**
Stationary; ThisIsYou requires MovementUpdate; a CombatVictim GUID must be non-empty with a
high type < 0x37; a MovementTransport GUID's high type must be one of {0, 2, 6, 8, 9, 10} and its
orientation in [0, 2π); a type-8/10 transport GUID (Creature/Pet) requires AreaTrigger; all nine
movement speeds must satisfy `|speed| >= 2.384e-7` (`0x14076c640`); MovementInfo's HasSpline bit
requires the block's HasSpline bit; GUID high types < 0x37 everywhere (`0x14076d970`).

### 3.1 MovementUpdate — `0x14070d420`

| # | read | field |
|---|---|---|
| 1 | MovementInfo (§5) | Status (`0x14070ceb0`) |
| 2-10 | float ×9 | WalkSpeed, RunSpeed, RunBackSpeed, SwimSpeed, SwimBackSpeed, FlightSpeed, FlightBackSpeed, TurnRate, PitchRate (+0xf8..+0x118; order per TC, all validated as speeds) |
| 11 | u32 | MovementForcesCount (`0x14079d200` resizes +0x348, element 0x38 bytes) |
| 12 | float | MovementForcesModMagnitude [inferred name] (+0x360) |
| 13-29 | float ×17 | advanced-flying constants (+0x364..+0x3a4), all validated finite. TC 3.4.3 writes, in order: AirFriction 2.0, MaxVel 65.0, LiftCoefficient 1.0, DoubleJumpVelMod 3.0, GlideStartMinHeight 10.0, AddImpulseMaxSpeed 100.0, MinBankingRate 90.0, MaxBankingRate 140.0, MinPitchingRateDown 180.0, MaxPitchingRateDown 360.0, MinPitchingRateUp 90.0, MaxPitchingRateUp 270.0, MinTurnVelocityThreshold 30.0, MaxTurnVelocityThreshold 80.0, SurfaceFriction 2.75, OverMaxDeceleration 7.0, LaunchSpeedCoefficient 0.4 [names/values inferred; the count of 1+17 floats is the client's] |
| 30 | byte | bit7 HasSpline (+0x340); bits 6..0 unused |
| 31 | MovementForce × count | §3.2 (`0x14070c920`) |
| 32 | if HasSpline: u32 | Spline ID (+0x120) |
| 33 | if HasSpline: float ×3 | Spline destination X, Y, Z (+0x124..) |
| 34 | if HasSpline: byte | bit7 HasSplineMove (+0x338) |
| 35 | if HasSplineMove | MovementSpline §3.4 (`0x14070ca70`) |

### 3.2 MovementForce — `0x14070c920` (also used by §5 consumers)

`PackedGuid ID; float Origin[3]; float Direction[3]; u32 TransportID; float Magnitude;
byte: bits7-6 Type (2 bits)`.

### 3.3 TransportLocation (MovementTransport) — `0x14070d2d0` (writer `0x14074c800`)

| read | field |
|---|---|
| PackedGuid | Guid |
| float ×4 | X, Y, Z, O |
| u8 | VehicleSeatIndex (signed) |
| u32 | MoveTime |
| byte | bit7 HasPrevMoveTime, bit6 HasVehicleRecID |
| u32 | PrevMoveTime (if bit7) |
| u32 | VehicleRecID (if bit6) |

### 3.4 MovementSpline — `0x14070ca70`

| # | read | field |
|---|---|---|
| 1 | u32 | Flags (MoveSplineFlag raw) |
| 2 | u32 | Elapsed (int32, TC `timePassed`) |
| 3 | u32 | Duration |
| 4 | float | DurationModifier |
| 5 | float | NextDurationModifier |
| 6 | 3 bytes = 24 bits | `Face` (2 bits), `HasFadeObjectTime` (1), `PointsCount` (16 bits, MSB first), `HasSplineFilter` (1), `HasSpellEffectExtraData` (1), `HasJumpExtraData` (1), `HasAnimTierTransition` (1), 1 unused bit |
| 7 | if HasSplineFilter | `u32 FilterKeysCount; {u32, u32} × count; byte: bits7-6 FilterFlags`. **Differs from TC** (TC also writes BaseSpeed/StartOffset/DistToPrevFilterKey/AddedToStart and int16/uint16 keys): the client reads only these. |
| 8 | Face == 1 (Spot) | float ×3 FaceSpot |
| 8 | Face == 2 (Target) | PackedGuid FaceGUID |
| 8 | Face == 3 (Angle) | float FaceDirection |
| 9 | HasFadeObjectTime | u32 FadeObjectTime |
| 10 | always | float ×3 × PointsCount | Points |
| 11 | HasSpellEffectExtraData | `PackedGuid TargetGUID; u32 SpellVisualID; u32 ProgressCurveID; u32 ParabolicCurveID` (`0x140714d20`) |
| 12 | HasJumpExtraData | `float JumpGravity; u32 StartTime; u32 Duration` |
| 13 | HasAnimTierTransition | `u32 TierTransitionID; u32 StartTime; u32 EndTime; u8 AnimTier` (`0x140714dc0`) |

### 3.5 AreaTrigger — `0x14070a1b0`

`u32 ElapsedMs; float RollPitchYaw[3];` then three bytes of flags, MSB first:

| bit # | flag | gates |
|---|---|---|
| 1 | AbsoluteOrientation | — |
| 2 | DynamicShape | — |
| 3 | Attached | — |
| 4 | FaceMovementDir | — |
| 5 | FollowsTerrain | — |
| 6 | Unk1 | — |
| 7 | HasTargetRollPitchYaw | float ×3 |
| 8 | HasScaleCurveID | u32 |
| 9 | HasMorphCurveID | u32 |
| 10 | HasFacingCurveID | u32 |
| 11 | HasMoveCurveID | u32 |
| 12 | HasAnimID | u32 |
| 13 | HasAnimKitID | u32 |
| 14 | HasUnk3 | one extra bit, in a **4th byte** (bit7), read before everything below |
| 15 | HasAnimProgress | u32 |
| 16 | HasAreaTriggerSphere | float Radius, RadiusTarget |
| 17 | HasAreaTriggerBox | float Extents[3], ExtentsTarget[3] |
| 18 | HasAreaTriggerPolygon | `0x14070ab10`: `u32 VerticesCount; u32 VerticesTargetCount; float Height; float HeightTarget; float[2] × VerticesCount; float[2] × VerticesTargetCount` |
| 19 | HasAreaTriggerCylinder | float ×6 (Radius, RadiusTarget, Height, HeightTarget, LocationZOffset, LocationZOffsetTarget) [names inferred] |
| 20 | HasDisk | float ×8 [names inferred] |
| 21 | HasBoundedPlane | float ×4 [names inferred] |
| 22 | HasAreaTriggerSpline | `0x14070ad10`: `u32 TimeToTarget; u32 ElapsedTimeForMovement; u16 PointsCount (two bytes, MSB first = 16 bits + flush); float[3] × PointsCount` |
| 23 | HasOrbit | `0x14070a970`: `byte: bit7 HasPathTarget, bit6 HasCenter, bit5 CounterClockwise, bit4 CanLoop; u32 TimeToTarget; i32 ElapsedTimeForMovement; u32 StartDelay; float Radius; float BlendFromRadius; float InitialAngle; float ZOffset; [PackedGuid PathTarget]; [float Center[3]]` |
| 24 | HasMovementScript | `0x14070ac70`: u32 ×4 (TC: SpellScriptID + Center xyz) [inferred] |

Read order after the flags: [Unk3 bit byte], **Spline**, TargetRollPitchYaw, ScaleCurveID,
MorphCurveID, FacingCurveID, MoveCurveID, AnimID, AnimKitID, AnimProgress, Sphere, Box,
Polygon, Cylinder, Disk, BoundedPlane, **MovementScript**, **Orbit**.

### 3.6 GameObject

`u32 WorldEffectID; byte: bit7 HasInt1; [u32 Int1]` (+0x750, +0x758, +0x754).

### 3.7 SmoothPhasing

`byte: bit7 ReplaceActive, bit6 StopAnimKits, bit5 HasReplaceObject; [PackedGuid ReplaceObject]`.

### 3.8 SceneObject

`byte: bit7 HasLocalScriptData, bit6 HasPetBattleFullUpdate;`
`[byte: bits7-1 = length (7 bits); char[length] LocalScriptData]` (read with `0x1402c9fe0`, not
NUL-terminated on the wire); `[PetBattleFullUpdate (`0x140716c40`, not detailed)]`.

### 3.9 ActivePlayer

`byte: bit7 HasSceneInstanceIDs, bit6 HasRuneState, bit5 HasActionButtons;`
`[u32 count; u32 SceneInstanceIDs[count]]`
`[RuneState (`0x140718f40`): u8 RechargingRuneMask(Start); u8 UsableRuneMask(Count); u32 CooldownsCount; u8 Cooldowns[count]]`
`[u32 ActionButtons[180]]` (0xb4 entries).

### 3.10 Conversation

`byte: bit7 HasTextureKitID; [u32 TextureKitID]`.

## 4. Values

### 4.1 ValuesCreate — vtable `+0x368` (e.g. Unit `0x1417a0bb0`, ActivePlayer `0x141713ce0`)

| read | field |
|---|---|
| u32 | Size: byte count of everything after this field (checked by `0x141707320`: *"jam mirror full update failure between %d and %d. Expected size %d received %d"*) |
| u8 | FieldFlags (visibility of the receiver): `0x01` Owner, `0x02` PartyMember, `0x04` UnitAll, `0x08` Empath — see updatefields.md for which fields each one adds |
| … | each data struct's **create** serialization, in inheritance order (ObjectData, then ItemData/UnitData/…, see table below) |

A create carries **every** field of every struct of the type, in declaration order, except
fields whose visibility flag the receiver does not have. No change masks.

### 4.2 ValuesUpdate — vtable `+0x370` (e.g. Unit `0x1417a0c60`, ActivePlayer `0x141713dd0`)

| read | field |
|---|---|
| u32 | Size: bytes after this field (the type mask included); the client resyncs to it on mismatch |
| u32 | ChangedObjectTypeMask: bit `(1 << TypeID)` of each struct present, in this order |
| … | the **update** serialization of each struct whose bit is set |

| struct | create reader | update reader | type-mask bit | offset in CG object |
|---|---|---|---|---|
| ObjectData | 0x1424fe880 | 0x142510e80 | 0x0001 (Object) | +0xd8 |
| ItemData | 0x1424fd470 | 0x14250f900 | 0x0002 (Item) | +0x178 |
| ContainerData | 0x1424fbf80 | 0x14250e390 | 0x0004 (Container) | +0x470 |
| UnitData | 0x1425019e0 | 0x142514530 | 0x0020 (Unit) | +0xd668 |
| PlayerData | 0x1424fec10 | 0x142511490 | 0x0040 (Player) | +0xdd48 |
| ActivePlayerData | 0x1424f5700 | 0x1425072c0 | 0x0080 (ActivePlayer) | +0xe600 |
| GameObjectData | 0x1424fc770 | 0x14250ee20 | 0x0100 (GameObject) | +0x1e0 |
| DynamicObjectData | 0x1424fc6b0 | 0x14250ed10 | 0x0200 (DynamicObject) | +0x188 |
| CorpseData | 0x1424fc390 | 0x14250e8a0 | 0x0400 (Corpse) | +0x158 |
| AreaTriggerData | 0x1424fbad0 | 0x14250dea0 | 0x0800 (AreaTrigger) | +0x510 |
| SceneObjectData | 0x142500fb0 | 0x142513a00 | 0x1000 (SceneObject) | +0x140 |
| ConversationData | 0x1424fc250 | 0x14250e6b0 | 0x2000 (Conversation) | +0x180 |

Structs per type (create order = update order):

| type | structs |
|---|---|
| Item | Object, Item |
| Container | Object, Item, Container |
| Unit | Object, Unit |
| Player | Object, Unit, Player |
| ActivePlayer | Object, Unit, Player, ActivePlayer |
| GameObject | Object, GameObject |
| DynamicObject | Object, DynamicObject |
| Corpse | Object, Corpse |
| AreaTrigger | Object, AreaTrigger |
| SceneObject | Object, SceneObject |
| Conversation | Object, Conversation |

Each struct's update starts with a fresh bit reader: its change mask (block mask, then one
32-bit word per set block — or a single word of N bits when the struct has ≤ 32 bits), then the
changed values, with flushes exactly where updatefields.md says.

## 5. MovementInfo (MovementStatus) — reader `0x14070ceb0`, writer `0x14074c3e0`

The same layout is used by the create block (§3.1), `SMSG_MOVE_UPDATE` (0x2DE0, reader
`0x14070ceb0`) and every `CMSG_MOVE_*` the client sends (e.g. `CMSG_MOVE_HEARTBEAT` 0x3A10 =
`0x140762bc0`: opcode, then the writer `0x14074c3e0`). The writer's calls give the types
(`0x1402ca480` = u32, `0x1402ca530` = float, `0x1402ca320` = u8, `0x141550bf0` = PackedGuid).

| # | type | field | struct offset |
|---|---|---|---|
| 1 | PackedGuid | MoverGUID | +0x0 |
| 2 | u32 | MovementFlags | +0xe8 |
| 3 | u32 | MovementFlags2 | +0xec |
| 4 | u32 | MovementFlags3 | +0xf0 |
| 5 | u32 | MoveTime | +0x10 |
| 6 | float ×4 | Position X, Y, Z, Facing | +0x14..+0x20 |
| 7 | float | Pitch | +0x80 |
| 8 | float | StepUpStartElevation | +0xa0 |
| 9 | u32 | RemoveForcesIDsCount | (array at +0xa8, 16-byte GUIDs) |
| 10 | u32 | MoveIndex | +0xc0 |
| 11 | PackedGuid × RemoveForcesIDsCount | RemoveForcesIDs | |
| 12 | byte | bit7 HasStandingOnGameObjectGUID (+0x38), bit6 HasTransport (+0x78), bit5 HasFall (+0x9c), bit4 HasSpline (+0xa4), bit3 HeightChangeFailed (+0xa5), bit2 RemoteTimeValid (+0xa6), bit1 HasInertia (+0xd8), bit0 HasAdvFlying (+0xe4) | |
| 13 | HasTransport | TransportLocation (§3.3) | +0x40 |
| 14 | HasStandingOnGameObjectGUID | PackedGuid | +0x28 |
| 15 | HasInertia | `u32 ID; float Force[3]; u32 Lifetime` | +0xc4..+0xd4 |
| 16 | HasAdvFlying | `float ForwardVelocity; float UpVelocity` | +0xdc, +0xe0 |
| 17 | HasFall | `u32 FallTime; float JumpVelocity (ZSpeed); byte: bit7 HasFallDirection; [float Direction.x (sin); float Direction.y (cos); float HorizontalSpeed]` | +0x84.. |

Validation (`0x14076d970`): position/facing/pitch finite; GUID high types < 0x37; transport,
fall, inertia and adv-flying blocks validated by their own helpers.

Note the 3.4.3 shape: three flag words (not 3.3.5's u32 + u16), a 128-bit packed mover GUID
first, and no per-opcode bit scrambling — every CMSG_MOVE_* body is exactly this struct
(some opcodes append fields after it; see docs/opcodes.md for each).
