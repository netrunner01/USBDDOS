# USBDDOS fork — status, TODO, and bug list
Updated: 2026-07-17 (housekeeping: PRs #40-42 confirmed MERGED; 5 stale merged source branches deleted from origin; session deliverables moved out of repo root; v2-line master-sync decided = Option A, cherry-picked #42 DPMI + #36 OOM + CHANGELOG onto release line, unpushed pending build-verify). Prior checkpoint 2026-06-06.

## Housekeeping done 2026-07-17
- **PRs #40/#41/#42 all MERGED** (verified via GitHub API). Their `pr/*`
  source branches were auto-deleted on merge — already gone, local and origin.
- **Deleted 5 stale merged source branches from origin** (`netrunner01` fork):
  `audit/malloc-graceful-failure` (#36), `audit/msc-error-visibility` (#37),
  `audit/usb-max-device-count` (#38), `docs/memory-manager-requirements` (#39),
  `fix/msc-maxlba-aliasing` (#35). All squash-merged upstream, which is why the
  batch-pr2.sh ancestor-guard couldn't remove them. Origin now holds only:
  `master`, `dbg/combined-trace`, `fix/msc-transport-rewrite`, `release/v1.0.0-alpha.2.1`.
- **Moved session deliverables out of repo root** to
  `../USBDDOS-session-deliverables/`: the BAR-0 `.patch` (redundant w/ `0c2fa7d`),
  `UDDCOMB6/7-map.txt`, `batch-pr.sh`, `batch-pr2.sh`. Repo root now clean.
- **v2-line master-sync DECIDED: Option A (cherry-pick, stay diverged).**
  The v2 line and upstream deliberately diverged at PR #32 and evolved in
  parallel; a full merge/rebase would conflict across 6 files (msc.c transport
  + hcd.h vtable the worst) to destabilize a field-validated line for little
  gain, since most upstream commits already have a v2 equivalent/superseder.
  Cherry-picked the two fixes v2 genuinely lacked (+ CHANGELOG):
  - `865a254` = upstream #42 DPMI nested-IRQ fix (clean; disjoint files pic.c/pic.h/dpmi_bc.h).
  - `10a7125` = upstream #36 OOM hardening (auto-merged clean; 65/6 == upstream, no double-guards).
  - `abd9683` = CHANGELOG entries for both (Unreleased/Fixed, with #42/#36 provenance).
  **NOT built here** (no OpenWatcom/DJGPP toolchain in the working env) — build
  both toolchains + QEMU smoke before pushing. **NOT pushed** (local ahead of
  origin by 3). **Consequence for the Test-G landing:** `fd29498` (the DPMI fix
  on dbg/combined-trace) is now REDUNDANT on the release line — when landing the
  bridge+INT15 stack, SKIP fd29498 (865a254 already carries it).
- **Superseded item:** v2-line master-sync/rebase full-merge decision.
  `origin/master` (`3600a8b`) leads `upstream/master` (`0d2d6b6`, = merge of #42)
  by 2 commits — Gap H5 (`f6369eb`, = merged #34) + Gap-K restore (`3600a8b`),
  both effectively upstream. Real question is whether to rebase the 25-commit v2
  line onto refreshed upstream/master. Scope separately when picked up.

## Branch state

| Branch | Head | Contents |
|---|---|---|
| `fix/msc-transport-rewrite` (release line) | `abd9683` (local; origin at `d39f839`, +3 unpushed) | P-series + `0c2fa7d` + **control-timeout** (`b178198`) + **device-pool fix** (`5d85eab`) + gitignore (`d39f839`) + **cherry-picked #42 DPMI** (`865a254`) + **#36 OOM** (`10a7125`) + CHANGELOG (`abd9683`) — unpushed 3 pending build-verify |
| `dbg/combined-trace` | `088a15c` | release base + traces + 8042 bridge (`4df062e`) + **DPMI nested-IRQ fix** (`fd29498`, = PR #42, merged) + **INT15h/C2xx PS/2 emulation** (`088a15c`) — awaiting metal Test G before landing on release line |
| ~~`pr/control-timeout`~~ | — | **PR #40 MERGED**; branch deleted |
| ~~`pr/device-pool`~~ | — | **PR #41 MERGED**; branch deleted |
| ~~`pr/v86-irq-reentrancy`~~ | — | **PR #42 MERGED**; branch deleted |
| `fix/control-timeout` | `ca960d2` | superseded — content on release line; deletable |

Landed this session: control-timeout and device-pool diffs moved to the
release line with field-validation noted in CHANGELOG (Unreleased section).
Upstream PRs for both: **deferred** by decision; diffs regenerated and
apply-clean (`control-timeout.diff`, `device-pool-overflow.diff`).

The 8042 mouse-bridge package (`mouse-8042-latch.diff`, 149 lines, hid.c
only) remains **on hold** on `dbg/combined-trace` pending combo5 field
results; not landed, not for upstream yet.

## combo5: entry-OBF bridge redesign — validated in QEMU

Commit `4df062e`. Health is judged at finalizer entry (aux byte still in
OBF across the inter-report gap = no consumer), not via in-ISR OUT_EMPTY
timeouts which are the normal outcome on every machine. Suspend after 3
consecutive stuck entries (aux-only drain + keyboard re-enable); one real
retry packet every 32nd report while suspended; resume when the retry is
gone by the next entry; per-byte OUT_EMPTY reduced to a 256-read pacing
bound; never stack a report onto an unconsumed byte.

QEMU validation (UDDCOMB5 binary, 3-phase bench): suspend PASS, honest
stay-suspended verdict PASS, cheap skips PASS, resume-state-machine PASS
with a POLLING aux consumer. **Field results (IBM 300GL, 2026-06-06):**
Test D (suspend) PASS; Test F (dock/disk) PASS incl. two-hub daisy chain
(pool to 8, held) and graceful pool-full refuse on three hubs; **Test E
(resume) BLOCKED — GP fault.** The fault is in the pseudo-DPMI V86
interrupt-reflection core, NOT the bridge: CTMOUSE arming the real aux
channel makes IRQ12 live for the first time with the driver resident, and
the next IRQ12 resumes the interrupted V86 (COMMAND.COM) frame as
protected-mode → #GP in DOS's own code → monitor deadloop (freeze).
Reproduced deterministically in qemu-rig (combo5 + AUXSINK, the armed-IRQ12
consumer); this is the same event earlier misattributed to a bench
"wedge" — AUXPOLL passed only because polling never arms IRQ12. See
`combo5-testE-fault-analysis.md`. **FIXED (2026-06-06):** root cause
confirmed by instrumentation — a second hardware IRQ (IRQ12, PM-IDT entry)
nests while the USB IRQ (V86/RM entry) is mid-service; the VCPI mode
switch's single client-save area is not reentrant, so the outer V86 frame
resumes as PM. Fix: reentrancy guard in `DPMI_HWIRQHandlerInternal` —
nested HW IRQs are masked, specifically-EOI'd (new `PIC_SendSpecificEOI`),
recorded, and re-delivered by unmasking after the outer service completes.
Commit `fd29498` on `dbg/combined-trace` (advanced from 4df062e); ported to
`pr/v86-irq-reentrancy` (`fdf2b20`) off upstream/master. Validated: the
deterministic reproducer goes from freeze-every-time to zero exceptions;
AUXPOLL 3-phase resume bench PASS; MSC disk mode no regression; both
toolchains clean. **combo6** (`v2.0.0-alpha.1.2-combo6`, UDDCOMB6.IMG)
packaged for the metal Test E re-run — the remaining leg that QEMU cannot
host (real CTMOUSE consumer). Tester floppy FDAUTO no longer echoes to the
COM1 DOS device (the "write fault" prompts).

## Test-bench findings (QEMU/SeaBIOS), for the rig docs

1. **SeaBIOS claims any USB mouse present at POST.** Its INT15/C2
   implementation routes every pointing-device call to its own USB-HID
   backend when `usb_mouse_active()`, never touching the i8042 (KCCB stays
   0x61, IRQ12 never enabled). CTMOUSE then reports "Installed at PS/2
   port" while actually riding the SeaBIOS USB path — which dies the
   moment USBDDOS takes over the host controller. Consequence: CTMOUSE can
   never serve as an i8042 aux consumer in QEMU while the USB mouse is
   cold-plugged.
2. **Hot-plug is not an escape:** USBDDOS has no hub PnP ("we don't
   support PnP yet", hub.c) — a `device_add` after residency is never
   enumerated. Also, with no claimable device at install the driver exits
   (AH=4Ch) instead of going resident; a cold-plugged USB disk anchors
   residency for such benches.
3. **Armed IRQ12 wedges the stack:** with the KCCB aux interrupt enabled
   and a V86 INT 74h handler installed, the first mid-inject IRQ12 hard-
   wedges QEMU+JEMM386+HDPMI (V86 reflection from inside the PM ISR
   context). Metal evidence says reflection-in-ISR works there (the
   field-proven keyboard path depends on IRQ1 reflection), so this is a
   bench artifact, not grounds for changing the inject's IRQ masking.
4. **Working methodology:** validate the bridge state machine with a
   foreground *polling* aux consumer (AUXPOLL.EXE: read 0x60 when status
   reads 0x21, no KCCB writes, no IRQ12). This exercises
   suspend/retry/resume end-to-end without touching reflection.

Bench tools added: `kccb.exe` (KCCB+status dump to COM1), `auxpoll.exe`
(polling consumer), `auxsink.exe` (IRQ12 consumer — wedges, kept as the
reproducer), `phase5*_drive.py`, QEMU `--trace pckbd_*` methodology.

## Field findings, IBM 300GL (bug15/16/17, 2026-06-05)

1. **"Device not found" is channel-side, not mouse-side (bug15).** A USB
   mouse known to speak PS/2 through an adapter still gets CTMOUSE's
   "ERROR: Device not found" when on USB: CTMOUSE probes the 8042 aux
   channel, which the BIOS leaves disabled with nothing on the PS/2
   connector at cold boot. Mouse protocol capability is irrelevant.
   Confirms the boot-conditional-aux model; no driver defect. (His
   capture's garbled head is an overwritten serial file across reboots.)
2. **SMM USB legacy support, gated on a USB keyboard (bug16).** With a
   USB keyboard attached at boot, the 300GL BIOS activates SMM-based USB
   legacy support and then drives any USB mouse natively (the 8042 is
   impersonated; wire protocol is USB). This is the metal counterpart of
   the QEMU/SeaBIOS bench finding: a BIOS layer owns the pointing path
   until the driver takes the HC. USBDDOS already disables the traps
   correctly at startup — uhci.c:78–84 writes 0x8F00 to Intel LEGSUP (PCI
   0xC0) — so legacy mode and USBDDOS are mutually exclusive by design.
   Test-E consequence: USB keyboard must be UNPLUGGED for bridge tests
   (instructions updated). GAP-B reminder stands: on VIA UHCI the legacy
   control is register 0x41, not PCI 0xC0; the Intel write is correct for
   this machine (PIIX).
3. **POST freeze: USB keyboard + hub/dock (bug17).** The legacy layer
   cannot enumerate through hubs; the machine freezes at boot. Topology
   constraint only: PS/2 keyboard for all dock tests. Docs item, no code.

## Upstream state (verified via GitHub API, 2026-06-05)

crazii merged the fork's entire small-PR queue into master overnight:
#34 (EHCI FS/LS bInterval slot index, from `h5-fs-bInterval-log-scale`),
#35 (MSC MaxLBA preserve), #36 (OOM hardening), #37 (MSC install-path
printfs), #38 (device-capacity reject logging), #39 (README memory
managers). All six confirmed `merged: true`. Follow-ups prepared (run `batch-pr2.sh` in Git Bash, dry-run by default):
the control-timeout and device-pool fixes are ported onto upstream/master
as single-commit branches `pr/control-timeout` (8ba2baf) and
`pr/device-pool` (10224dc) — conflicts from the v2 vtable divergence
resolved (only `AbortControl` brought over, not P7's
`ResetEndpointToggle`; the dead-`DeviceCount` removal re-verified against
upstream, with the freshly merged visibility hint folded into the live
pool-full log) — both build clean on both toolchains and boot-smoke-tested
on the upstream code base; the script also deletes the six merged source
branches (ancestor-guarded) and imports/fast-forwards this session's
branches from `usbddos-2026-06-06.bundle`. A third PR is in the manifest:
`pr/v86-irq-reentrancy` (`fdf2b20`), the nested-IRQ DPMI fix. Remaining: eventual master-sync/rebase
decision for the v2 line.

## Poll-loop root cause (bug14 tail) — RESOLVED as by-design

The endless post-resident `REQ a3 bmReq=a3 bReq=00 wIdx=0002` lines are
the MSC DOS driver's removal detection: msc.c calls
`pHub->GetPortStatus()` on the disk's own hub port **on every DOS request**
(MEDIACHECK/BUILD_BPB/READ/WRITE), and for a hub-attached disk each check
is a GET_PORT_STATUS control transfer. DOS issues MEDIACHECK continuously,
hence the stream. (`wIdx` is 1-based: 0x0002 = the disk's port, log-port 1
— not the removed device's port as first guessed.) Harmless; the 4.5 MB
copy ran through it. Improvement (low priority): throttle the check with a
BIOS-tick cache (e.g. re-check at most every ~0.5 s), which also silences
the debug log.

## Known limitations (for README/TESTING wording)

- **USB mouse bridge needs a live aux channel.** The PS/2 injection path
  requires the keyboard controller's aux port to have been enabled by the
  BIOS at boot. On machines whose BIOS auto-detects the pointing device
  (e.g. IBM 300GL), boot with a PS/2 mouse plugged into the PS/2 port; a
  BIOS setup option forcing the port "Enabled" also works where offered.
  Without a live aux channel the driver suspends the bridge (keyboard
  unaffected) and retries periodically.
- **Motion quality on real 8042s.** The injector writes packet bytes
  back-to-back; a real 1-byte-deep controller can drop/garble bytes if the
  consumer is slow. A coalescing ring with paced injection is the planned
  fix (below).
- **The FreeDOS test floppy is a diagnostic environment** (JEMM386 +
  HDPMI loaded). Some software objects to V86 (e.g. Lemmings under
  JEMM386); run games from a plain DOS setup.

## Field findings round 2 (bug26-29, 2026-06-06 afternoon)

1. **Crash fix robustness confirmed.** Three further runs (PS/2 mouse
   yanked pre-load; absent entirely, with and without capture): "cannot
   seem to reproduce the system hanging state anymore." PR #42 is
   multi-run validated on metal.
2. **NEW GAP — no aux device-protocol emulation.** With no physical PS/2
   mouse, CTMOUSE reports "Error: Device not found" and never installs.
   Verified in code: hid.c implements only 0xD2/0xD3 fake-input injection
   and KCCB access; there is no 0xD4 command interception, so nothing
   ACKs the consumer's reset/enable probe (0xFF -> expects 0xFA + 0xAA).
   The bridge can ride alongside a real mouse but cannot substitute for
   one. This caps the bridge's usefulness on exactly the machines that
   need it most (no PS/2 mouse present).
3. **Proof point: the INT15h/C2xx software-emulation architecture works
   on this machine.** Tester ran Bret Johnson's USBUHCI + USBMOUSE, then
   CTMOUSE ("detected the mouse"), then EDIT: USB mouse confirmed
   working. CTMOUSE is INT15-based; USBMOUSE emulates the INT15h/C2xx
   pointing-device services in software, so no 8042 writes, no IRQ12, no
   nested-IRQ reentrancy, and no physical mouse required. This is the
   strongest candidate architecture for the resume/bridge redesign
   decision (pending), ahead of non-ISR 8042 injection or deferred-IRQ
   replay.
4. Watch item: one capture (bug29) opened with ~100 repeats of the first
   ~30 bytes of the startup banner before a clean full run. A driver-side
   restart loop would repeat the whole init sequence, not a 30-byte
   prefix, so this looks capture-side (Pi serial). Monitor; no action.
5. The tester now has a working USB-mouse path on his machine (Bret
   stack), which removes schedule pressure from the redesign decision.

## TODO (priority order)

1. **DONE: combo6 Test E ran on the IBM 300GL.** Crash fix validated
   (multi-run); resume did not fire (see root cause below); CTMOUSE needs
   a physical mouse (device-emulation gap). Tester reply pending — draft
   ready (issue2-reply-combo6-results.txt).
2. **DONE 2026-07-17: Upstream PRs #40/#41/#42 all MERGED.** pr/* source
   branches auto-deleted. Also deleted 5 older stale merged source branches
   (#35-39) that the ancestor-guard missed. Remaining under this item:
   the v2-line master-sync/rebase decision (still open — see Housekeeping section).
3. **Sync check on Art's clone:** `fix/msc-transport-rewrite` fast-forward
   to `36abbc7` (fetch bundle → merge --ff-only → push origin) — commands
   issued, completion unconfirmed.
4. **RESOLVED: option (A) implemented (`088a15c`).** INT15h/C2xx
   pointing-device emulation: RMCB hook on IVT[15h]; C200-C207 emulated
   per RBIL + CuteMouse source (C205 accepts only the 3-byte protocol so
   wheel probes fall back; C208/C209 fail AH=1); C207 handler far-called
   per report via a 21-byte RM thunk in a DOS-allocated block (the
   program's own conventional image is released at TSR - keep is 0);
   equipment word bit 2 set; non-C2 calls chain to the old vector. The
   8042 inject path remains as fallback when no INT15 handler is
   registered; its suspend logic is unchanged. End-to-end QEMU PASS:
   CTMOUSE installs with NO mouse device present (rc=0), MICKEY (INT 33h
   AX=000Bh) tracks USB motion, signs correct, 0 exceptions. Regressions
   PASS: no-client 8042 fallback suspends; AUXSINK crash-repro clean;
   disk mode clean; both toolchains -Werror clean.
   **combo7 (UDDCOMB7.IMG, v2.0.0-alpha.1.3-combo7) packaged**: combo6
   image + new driver + MICKEY.EXE; boot-smoked. Tester ask = Test G
   (no PS/2 mouse needed at all).
5. **After Test G passes on metal:** land bridge (4df062e) + DPMI fix
   (fd29498) + INT15 emulation (088a15c) on the release line; CHANGELOG;
   consider upstreaming the INT15 emulation as the headline PR (crazii's
   own release notes recommend the CTMOUSE pairing). Known limitations to
   document: single static DPMI_REG for the INT15 hook (theoretical
   nesting hazard, e.g. AH=4Fh during a chained AH=86h wait - same risk
   class as the codebase norm); equipment bit set whenever a USB mouse is
   present; wheel not emulated (3-byte only); the 8042 fallback retains
   the old no-resume limitation for raw-8042 consumers (superseded
   path). Motion smoothness (report coalescing at the INT15 layer if
   needed) is the next polish item, replacing the old 8042 coalescing
   ring.
6. **Wave C upstream candidate: BAR-0 skip (`0c2fa7d`).** Landed at the
   base of the fork's v2 line, absent upstream — upstream still hangs on
   machines with USB disabled in BIOS (BAR reads 0, driver does I/O at 0).
   On go: cherry-pick onto upstream/master, build/smoke, add as fourth
   manifest entry in batch-pr2.sh.
7. Pool ceiling: `USB_MAX_DEVICE_COUNT` 8 is exceeded by three daisy-chained
   hubs (bug23) — graceful refuse confirmed in field, but the disk lands
   past the ceiling. Options: raise to 12–16 (~72 B/slot resident), or
   document as tunable. Low priority; tester called it an edge case.
8. MSC removal-detection throttle (BIOS-tick cache) — also silences the
   per-DOS-request REQ-a3 log stream (BUG-14).
9. Hub PnP (hot-plug) support — currently absent ("we don't support PnP
   yet"); needed for post-resident topology change and better benches.
10. Rig docs: fold the SeaBIOS/PnP/IRQ12 bench findings + the
    AUXSINK-reproducer methodology into TESTING.md.
11. Housekeeping (Art's clone): move session deliverables out of the repo
    root (sibling folder); the stray BAR-0 .patch file is redundant with
    `0c2fa7d` and can be archived.

## Bug list

| ID | Status | Summary |
|---|---|---|
| GP fault, nested HW IRQ (combo5 Test E) | **fixed** `fd29498` / PR #42 | V86 frame resumed as PM when a 2nd IRQ nested mid-mode-switch; deferral guard added; **field-validated multi-run (bug24-29)** |
| Bridge resume never fires w/ IRQ12 consumer | **superseded** by `088a15c` | Edge-triggered IRQ12 lost by safe deferral (proven); INT15h/C2xx emulation removes IRQ12 from the mouse path entirely; 8042 fallback documented as no-resume for raw-8042 consumers |
| No aux device-protocol emulation (CTMOUSE "Device not found") | **fixed** by `088a15c` | INT15 emulation answers the driver directly; no physical mouse needed; QEMU end-to-end PASS, metal = Test G |
| COM1 "write fault" on tester floppy | fixed in UDDCOMB6.IMG | FDAUTO echoed to the COM1 DOS device; capture rig rejects handshake-less writes; echoes removed (driver's direct-UART logging unaffected) |
| BUG-14 (poll loop) | closed/by-design | MSC per-request GET_PORT_STATUS removal detection; throttle = TODO 8 |
| BUG-15 | open, low | Driver exits instead of residency with only unclaimed devices (hub-only topology); arguably correct, surprises benches |
| BUG-16 | open, medium | No hub PnP — hot-plugged devices never enumerated (TODO 9) |
| 1-deep 8042 garbling | open, medium | Known limitation; coalescing ring (TODO 5) |
| Pool ceiling vs deep hub chains | open, low | bug23: 3 hubs exceed pool of 8; graceful refuse works; tunable (TODO 7) |
| BUG-17 (POST freeze, USB kbd + hub) | closed/firmware | 300GL BIOS legacy layer can't enumerate hubs; topology constraint documented, no code |
| Jemm386 + Lemmings | docs-only | Boot floppy is diagnostic env; documented |
