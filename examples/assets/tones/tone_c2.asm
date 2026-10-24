; tone_c2 — a complete, self-contained wave-channel (CH3) driver sounding C5 (~523 Hz). Every voice
; runs on its own fresh VM, so the driver carries its whole hardware setup — it never depends on
; another routine having configured the chip. Order matters for a clean start: the APU + wave RAM +
; DAC + volumes are set while the channel is UNTRIGGERED (silent — wave RAM is safe to write, the
; DAC-on step lands on silence), then one frame of settle lets the output center before the single
; trigger starts the note cleanly (no pop rides the onset).
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $01
ldh [$FF30], a
ld a, $23
ldh [$FF31], a
ld a, $45
ldh [$FF32], a
ld a, $67
ldh [$FF33], a
ld a, $89
ldh [$FF34], a
ld a, $AB
ldh [$FF35], a
ld a, $CD
ldh [$FF36], a
ld a, $EF
ldh [$FF37], a
ld a, $FE
ldh [$FF38], a
ld a, $DC
ldh [$FF39], a
ld a, $BA
ldh [$FF3A], a
ld a, $98
ldh [$FF3B], a
ld a, $76
ldh [$FF3C], a
ld a, $54
ldh [$FF3D], a
ld a, $32
ldh [$FF3E], a
ld a, $10
ldh [$FF3F], a
ld a, $80
ldh [rNR30], a    ; CH3 DAC on (channel still untriggered, so it stays silent)
ld a, $FF
ldh [rNR51], a    ; panning: all channels to L and R
ld a, $77
ldh [rNR50], a    ; master volume max on L/R
ld a, $00
ldh [rNR31], a    ; length (length-disabled at trigger, so unused)
ld a, $40
ldh [rNR32], a    ; CH3 output level 50% — moderate
ld bc, $09CC      ; settle: 2508 iterations x 28 cycles = one frame (70224) of silence
.settle:
dec bc
ld a, b
or c
jr nz, .settle
ld a, $83
ldh [rNR33], a    ; frequency low — C5
ld a, $87
ldh [rNR34], a    ; trigger (bit 7) + frequency high
.idle:
jr .idle
