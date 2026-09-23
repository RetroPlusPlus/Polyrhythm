#pragma once

// The demo cartridge every SNES example hosts — authored here as 65816 source, assembled in process, so
// a reader follows the program rather than a committed binary. It draws one sprite and moves it with the
// pad: the d-pad steps the sprite, A and B recolour it, and Start writes the battery save. The whole
// program is 65816.
//
// The picture is one 32x32 sprite of a single colour over the backdrop, with no background layers, so the
// only thing on screen is the sprite the pad drives. The native NMI reads the auto-joypad registers each
// frame and moves the sprite; the reset code runs in forced blank, where video memory is free to fill.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "snaggletooth/snes/snes.h"  // snaggletooth::Region

namespace retropp::examples::snes {

// The program. Reset (in forced blank) uploads the sprite tile and its colour, hides every sprite, turns
// the object screen on, and enables the vertical-blank NMI + the auto-joypad read; the NMI reads the pad
// and drives sprite 0.
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

        ; Fill VRAM tiles 0-63 with a solid block of colour 1, so a 32x32 sprite (a 4x4 grid of tiles) is
        ; solid whichever tiles it reads. A16 lets one store write a whole VRAM word (low to $2118, high to
        ; $2119) and step the address; each tile is eight rows of plane 0 set (colour 1) then eight blank.
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

        ; The sprite's colour: object palette 0, colour 1 is CGRAM word 129, set white.
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
        LDA #$81
        STA !$4200              ; NMITIMEN: vertical-blank NMI + auto-joypad read
idle:   BRA idle

        ORG $00:8100
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
// battery save and names the native NMI vector.
inline std::vector<std::uint8_t> demoCartridge(snaggletooth::Region region = snaggletooth::Region::Ntsc) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(kDemoCartridgeSource, "snes_demo.asm");
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        const std::size_t at = range.start - 0x8000u;
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    rom[0x7FC0u + 0x2Au] = 0x00u;  // native NMI vector -> $8100
    rom[0x7FC0u + 0x2Bu] = 0x81u;
    rom[0x7FC0u + 0x16u] = 0x02u;  // chipset: RAM + battery
    rom[0x7FC0u + 0x18u] = 0x03u;  // save-size code: 8 KB
    rom[0x7FC0u + 0x19u] = region == snaggletooth::Region::Pal ? 0x02u : 0x01u;  // country byte
    return rom;
}

}  // namespace retropp::examples::snes
