#pragma once

// The demo cartridge every SNES example hosts — authored here as 65816 and SPC700 source, assembled in
// process, so a reader follows the program rather than a committed binary. It draws one sprite and
// moves it with the pad: the d-pad steps the sprite, A and B recolor it, and Start writes the battery
// save. It plays a four-note round on the sound chip: at reset the main program uploads a driver to
// the audio unit through the boot stub's own protocol, and the driver steps a looping square wave
// through A4, C#5, E5 and A5, half a second each, on the audio unit's timer.
//
// The picture is one 32x32 sprite of a single color over the backdrop, with no background layers, so
// the only thing on screen is the sprite the pad drives. The native NMI reads the auto-joypad registers
// each frame and moves the sprite; the reset code runs in forced blank, where video memory is free to
// fill.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "spc700/spc700_asm.h"
#include "examples/common.h"
#include "snaggletooth/snes/snes.h"  // snaggletooth::Region

namespace retropp::examples::snes {

// The sound driver. It runs on the audio unit at $0200 once uploaded: it points the DSP at the sample
// directory, sets voice 0 up on the square wave at full gain, unmutes, starts timer 0 at 32 ticks a
// second, and then steps the voice's pitch through four notes, sixteen ticks each. The wave is one BRR
// block of sixteen samples — eight high, eight low — that loops on itself; pitch $1000 plays a block at
// 32'000 Hz, so a note's pitch is its frequency x 16 / 32'000 x 4096.
inline constexpr std::string_view kDemoSoundSource = R"asm(
        ORG $0200
        MOV $F2,#$5D
        MOV $F3,#$03            ; DIR: the sample directory is page $03
        MOV $F2,#$04
        MOV $F3,#$00            ; V0SRCN: voice 0 plays source 0
        MOV $F2,#$00
        MOV $F3,#$40            ; V0VOLL
        MOV $F2,#$01
        MOV $F3,#$40            ; V0VOLR
        MOV $F2,#$07
        MOV $F3,#$7F            ; V0GAIN: direct, full
        MOV $F2,#$0C
        MOV $F3,#$60            ; MVOLL
        MOV $F2,#$1C
        MOV $F3,#$60            ; MVOLR
        MOV $F2,#$6C
        MOV $F3,#$20            ; FLG: unmuted, echo writes off
        MOV $F2,#$4C
        MOV $F3,#$01            ; KON: voice 0 starts, at the pitch the loop below sets
        MOV $FA,#250            ; T0DIV: timer 0 counts 250 of its 8 kHz ticks, 32 times a second
        MOV $F1,#$01            ; CONTROL: timer 0 on, the boot window unmapped
        MOV X,#$00              ; the note
note:   MOV $F2,#$02
        MOV A,!lo+X
        MOV $F3,A               ; V0PITCHL
        MOV $F2,#$03
        MOV A,!hi+X
        MOV $F3,A               ; V0PITCHH
        MOV Y,#$10              ; sixteen timer ticks: half a second a note
wait:   MOV A,$FD               ; T0OUT: the ticks since the last read, which clears it
        BEQ wait
tick:   DEC Y
        BEQ next
        DEC A
        BNE tick
        BRA wait
next:   INC X
        MOV A,X
        AND A,#$03              ; four notes, round and round
        MOV X,A
        BRA note
lo:     DB $85,$6F,$46,$0A      ; A4, C#5, E5, A5: pitch = f x 16 / 32'000 x 4096
hi:     DB $03,$04,$05,$07
        ORG $0300
        DW $0304,$0304          ; directory entry 0: the block right after it, looping on itself
        DB $C3                  ; shift 12, filter 0, loop and end: one block is the whole wave
        DB $77,$77,$77,$77,$99,$99,$99,$99   ; eight samples at +7, eight at -7: a square, sixteen samples a cycle
)asm";

// The program. Reset (in forced blank) uploads the sprite tile and its color, hides every sprite, turns
// the object screen on, uploads the sound driver to the audio unit and starts it, and enables the
// vertical-blank NMI + the auto-joypad read; the NMI reads the pad and drives sprite 0.
inline constexpr std::string_view kDemoCartridgeSource = R"asm(
        ORG $00:8000
        EMULATION
        CLC
        XCE                     ; native mode
        SEP #$30                ; 8-bit accumulator and index registers

        LDA #120
        STA $10                 ; sprite 0 X, near the middle of the screen
        LDA #104
        STA $11                 ; sprite 0 Y

        ; Fill VRAM tiles 0-63 with a solid block of color 1, so a 32x32 sprite (a 4x4 grid of tiles) is
        ; solid whichever tiles it reads. A16 lets one store write a whole VRAM word (low to $2118, high to
        ; $2119) and step the address; each tile is eight rows of plane 0 set (color 1) then eight blank.
        LDA #$80
        STA !$2115              ; VMAIN: the address steps one word after the high byte
        STZ !$2116              ; VMADDL
        STZ !$2117              ; VMADDH -> VRAM word 0
        REP #$20                ; 16-bit accumulator
        LDX #$00                ; tile counter
tfill:  LDY #$00
trow:   LDA #$00FF
        STA !$2118              ; a row: plane 0 all set, plane 1 clear
        INY
        CPY #$08
        BNE trow
        LDY #$00
tblank: STZ !$2118              ; planes 2 and 3 clear
        INY
        CPY #$08
        BNE tblank
        INX
        CPX #$40                ; 64 tiles
        BNE tfill
        SEP #$20                ; back to 8-bit accumulator

        ; The sprite's color: object palette 0, color 1 is CGRAM word 129, set white.
        LDA #$81
        STA !$2121              ; CGADD
        LDA #$FF
        STA !$2122              ; CGDATA low
        LDA #$7F
        STA !$2122              ; CGDATA high -> 0x7FFF

        ; Hide every sprite below a 224-line screen; sprite 0 is placed each frame by the NMI.
        STZ !$2102              ; OAMADDL
        STZ !$2103              ; OAMADDH
        LDX #$00
hide:   STZ !$2104              ; X
        LDA #$E0
        STA !$2104              ; Y = 224: a 32x32 sprite here is wholly below the 224-line screen and
        ;                         does not wrap (224+31 = 255), so an unused sprite never shows
        STZ !$2104              ; tile
        STZ !$2104              ; attributes
        INX
        CPX #$80
        BNE hide
        LDX #$00
hitab:  STZ !$2104              ; the 32-byte high table: X bit 8 clear, small size
        INX
        CPX #$20
        BNE hitab

        LDA #$A0
        STA !$2101              ; OBSEL: object tiles from VRAM word 0, sprites are 32x32
        LDA #$10
        STA !$212C              ; TM: objects on the main screen
        LDA #$0F
        STA !$2100              ; INIDISP: screen on, full brightness
        ; The cartridge's sound: the audio unit's upload stub waits with $AA/$BB in ports 0 and 1. Send
        ; the driver to $0200 and its directory and sample to $0300, one acknowledged byte at a time,
        ; then start it at $0200. The interrupt is still off, so nothing disturbs X while it counts.
ready:  LDA !$2140
        CMP #$AA
        BNE ready
        LDA !$2141
        CMP #$BB
        BNE ready
        LDA #$00
        STA !$2142              ; destination $0200, low
        LDA #$02
        STA !$2143              ; high
        LDA #$01
        STA !$2141              ; a transfer, not a start
        LDA #$CC
        STA !$2140              ; the kick
kick:   CMP !$2140
        BNE kick                ; acknowledged
        LDX #$00
prog:   LDA !$A000,X            ; the driver's next byte
        STA !$2141
        TXA
        STA !$2140              ; its index
ackp:   CMP !$2140
        BNE ackp
        INX
        CPX #$80                ; the driver's 128 bytes: the program and the zeros after it
        BNE prog
        LDA #$00
        STA !$2142              ; destination $0300, low
        LDA #$03
        STA !$2143
        LDA #$01
        STA !$2141              ; a transfer
        LDA #$81                ; two past the last index
        STA !$2140
ackd:   CMP !$2140
        BNE ackd
        LDX #$00
data:   LDA !$A100,X            ; the directory's and the sample's next byte
        STA !$2141
        TXA
        STA !$2140
ackq:   CMP !$2140
        BNE ackq
        INX
        CPX #$10                ; sixteen bytes: the directory entry and the one block
        BNE data
        LDA #$00
        STA !$2142              ; start at $0200, low
        LDA #$02
        STA !$2143
        LDA #$00
        STA !$2141              ; zero starts the program
        LDA #$11                ; two past the last index
        STA !$2140
acks:   CMP !$2140
        BNE acks

        LDA #$81
        STA !$4200              ; NMITIMEN: vertical-blank NMI + auto-joypad read
idle:   BRA idle

        ORG $00:8200
        A8
        X8
nmi:    LDA !$4210              ; RDNMI: acknowledge the vertical-blank NMI
wait:   LDA !$4212              ; HVBJOY
        AND #$01
        BNE wait                ; until the auto-read has finished
        LDA !$4219              ; JOY1 high byte: Right,Left,Down,Up in bits 0-3
        STA $12
        AND #$01
        BEQ noR
        INC $10                 ; Right
noR:    LDA $12
        AND #$02
        BEQ noL
        DEC $10                 ; Left
noL:    LDA $12
        AND #$04
        BEQ noD
        INC $11                 ; Down
noD:    LDA $12
        AND #$08
        BEQ noU
        DEC $11                 ; Up
noU:    LDA !$4218              ; JOY1 low byte: A in bit 7
        AND #$80
        BEQ noA
        LDA #$81                ; A held: the sprite turns red
        STA !$2121
        LDA #$1F
        STA !$2122
        STZ !$2122              ; 0x001F
noA:    LDA $12
        AND #$80                ; B in the high byte's bit 7
        BEQ noB
        LDA #$81                ; B held: the sprite turns green
        STA !$2121
        LDA #$E0
        STA !$2122
        LDA #$03
        STA !$2122              ; 0x03E0
noB:    LDA $12
        AND #$10                ; Start
        BEQ noStart
        LDA #$5A
        STA $70:0000            ; Start writes the battery save window
noStart:
        STZ !$2102              ; OAMADDL: sprite 0
        STZ !$2103
        LDA $10
        STA !$2104              ; X
        LDA $11
        STA !$2104              ; Y
        STZ !$2104              ; tile 0
        LDA #$30                ; priority 3, palette 0
        STA !$2104
        RTI
)asm";

// The assembled cartridge for `region`, over a one-bank LoROM image whose header declares an 8 KB
// battery save and names the native NMI vector. The sound driver is assembled beside the main program
// and laid at $00:A000 (its code) and $00:A100 (its directory and sample), where the reset code reads
// it from to upload it.
inline std::vector<std::uint8_t> demoCartridge(snaggletooth::Region region = snaggletooth::Region::Ntsc) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(kDemoCartridgeSource, "snes_demo.asm");
    const snaggletooth::assembler::Assembly sound =
        snaggletooth::assembler::assembleSpc700(kDemoSoundSource, "snes_demo_sound.asm");
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        const std::size_t at = range.start - 0x8000u;
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    for (const auto& range : sound.ranges) {
        // Audio-unit address $0200 lands at $00:A000 and $0300 at $00:A100. The reset code uploads 128
        // bytes from the first and 16 from the second, so each range has to fit its upload.
        const std::size_t limit = range.start == 0x0200u ? 0x80u : 0x10u;
        if (range.bytes.size() > limit) {
            throw std::runtime_error("the sound driver does not fit the bytes its upload sends");
        }
        const std::size_t at = 0x1E00u + range.start;
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    rom[0x7FC0u + 0x2Au] = 0x00u;  // native NMI vector -> $8200
    rom[0x7FC0u + 0x2Bu] = 0x82u;
    rom[0x7FC0u + 0x16u] = 0x02u;  // chipset: RAM + battery
    rom[0x7FC0u + 0x18u] = 0x03u;  // save-size code: 8 KB
    rom[0x7FC0u + 0x19u] = region == snaggletooth::Region::Pal ? 0x02u : 0x01u;  // country byte
    return rom;
}

}  // namespace retropp::examples::snes
