# Wait for MSX-DOS2 to finish booting, then type the player command.  The
# carriage return is sent separately because the openMSX type command can
# leave a DOS2 line editor with the text filled in but not submitted when the
# command and CR are in the same type request.
source "/Volumes/EXT_SSD/AI/libkss_conversion_tools/openmsx_debug/openmsx_trace.tcl"

set power on

proc type_kssplay_command {} {
    type "KSSPLAY.COM F1.KSS 54"
}

proc submit_kssplay_command {} {
    type "\r"
}

after realtime 12 type_kssplay_command
after realtime 15 submit_kssplay_command

# Keep a machine-side snapshot for diagnosing a DOS2 loader that stops after
# the command has been submitted.  The screen remains visible in the normal
# GUI run; these files let us inspect the exact CPU state and text screen.
proc dump_kssplay_state {} {
    set output [open "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-msx-state.txt" w]
    foreach regname {PC SP AF BC DE HL IX IY} {
        puts $output "$regname=[format %04X [reg $regname]]"
    }
    set pc [reg PC]
    puts $output "PC_BYTES=[binary encode hex [debug read_block memory $pc 0x20]]"
    foreach port {0xA8 0xFC 0xFD 0xFE 0xFF} {
        puts $output "PORT_[format %02X $port]=[format %02X [debug read ioports $port]]"
    }
    puts $output "VDP_STATUS=[format %02X [debug read ioports 0x99]]"
    foreach address {0x0100 0x0200 0x4000 0x5F00 0x8000 0x9800 0xC000 0xDFE0 0xE000 0xF200 0xF37A 0xF380 0xF3A0 0xF700 0xF704 0xFD9F} {
        set bytes [debug read_block memory $address 0x40]
        puts $output "MEM_[format %04X $address]=[binary encode hex $bytes]"
    }
    foreach address {0x0000 0x0001 0x0009 0x0024 0x0028 0x0030 0x0038 0x0093 0x0096} {
        set bytes [debug read_block memory $address 8]
        puts $output "VECTOR_[format %04X $address]=[binary encode hex $bytes]"
    }
    set vram [debug read_block {physical VRAM} 0 0x8000]
    set raw [open "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-msx-screen.bin" w]
    fconfigure $raw -translation binary -encoding binary
    puts -nonewline $raw $vram
    close $raw
    if {![catch {debug read_block {Empty Konami SCC Cartridge SCC SCC} 0 0x100} scc]} {
        set sccraw [open "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-scc.bin" w]
        fconfigure $sccraw -translation binary -encoding binary
        puts -nonewline $sccraw $scc
        close $sccraw
    }
    set mainram_size [debug size {Main RAM}]
    set physical [open "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-mainram.txt" w]
    puts $physical "MAIN_RAM_SIZE=$mainram_size"
    set segment_count [expr {$mainram_size / 0x4000}]
    for {set segment 0} {$segment < $segment_count} {incr segment} {
        set base [expr {$segment * 0x4000}]
        set head [debug read_block {Main RAM} $base 0x40]
        set load [debug read_block {Main RAM} [expr {$base + 0x1F00}] 0x40]
        set wrapper [debug read_block {Main RAM} [expr {$base + 0x337A}] 0x40]
        puts $physical "SEGMENT_[format %02X $segment]_HEAD=[binary encode hex $head]"
        puts $physical "SEGMENT_[format %02X $segment]_LOAD=[binary encode hex $load]"
        puts $physical "SEGMENT_[format %02X $segment]_WRAPPER=[binary encode hex $wrapper]"
    }
    puts $physical "PLAYER_CONFIG=[binary encode hex [debug read_block {Main RAM} [expr {4 * 0x4000 + 0x1FE0}] 0x48]]"
    puts $physical "PLAYER_VARS=[binary encode hex [debug read_block {Main RAM} [expr {4 * 0x4000 + 0x640}] 0x40]]"
    foreach segment {0 2 7} {
        set full [debug read_block {Main RAM} [expr {$segment * 0x4000}] 0x4000]
        set fullraw [open "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-segment-[format %02X $segment].bin" w]
        fconfigure $fullraw -translation binary -encoding binary
        puts -nonewline $fullraw $full
        close $fullraw
    }
    puts $physical "TRAMPOLINE=[binary encode hex [debug read_block {Main RAM} [expr {0 * 0x4000 + 0x0133}] 0x20]]"
    close $physical
    close $output
}

after realtime 20 dump_kssplay_state
after realtime 17 {
    trace_mapper none
    trace_pc_range 0x5F80 0x5FA0
    trace_pc_enabled on
    trace_disasm on
    trace_mem_ranges {{0x9800 0x98FF} {0xB800 0xB8FF}}
    trace_io_ranges {{0xA0 0xA1} {0x98 0x9B} {0xFC 0xFF}}
    trace_start /Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-audio-trace.log
    soundlog start /Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-audio.wav
    after realtime 10 {
        soundlog stop
        trace_stop
    }
}
