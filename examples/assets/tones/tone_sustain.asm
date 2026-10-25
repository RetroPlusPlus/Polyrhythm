; tone_sustain — a self-contained CONTINUOUS pulse tone (CH2, ~512 Hz). Its envelope pace is 0, so the
; envelope is DISABLED and the volume stays constant — the tone plays forever once triggered and its output
; is never silent. For the auto-close NEGATIVE test: a still-sounding voice must NOT auto-close, even when
; tagged Sfx (the high-pass-centered square oscillates through 0 but never holds (0,0)).
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $80
ldh [rNR21], a    ; CH2 duty 50%
ld a, $F0
ldh [rNR22], a    ; envelope: volume 15, pace 0 -> DISABLED, constant volume (never decays to silence)
ld a, $00
ldh [rNR23], a    ; frequency low
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
