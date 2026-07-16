proc dump_f1_state {} {
    set output [open "/tmp/f1-msx-state.txt" w]
    foreach regname {PC SP AF BC DE HL IX IY} {
        puts $output "$regname=[format %04X [reg $regname]]"
    }
    foreach port {0xFC 0xFD 0xFE 0xFF} {
        puts $output "PORT_[format %02X $port]=[format %02X [debug read ioports $port]]"
    }
    foreach address {0x0038 0x0200 0x4000 0x5000 0x5F00 0x8000 0xE160 0xEFC0} {
        set bytes [debug read_block memory $address 0x40]
        puts $output "MEM_[format %04X $address]=[binary encode hex $bytes]"
    }
    puts $output "PSG_REGS=[binary encode hex [debug read_block {PSG regs} 0 0x10]]"
    set bank [debug read_block {Main RAM} [expr {14 * 0x4000}] 0x4000]
    set raw [open "/tmp/f1-msx-segment14.bin" w]
    fconfigure $raw -translation binary -encoding binary
    puts -nonewline $raw $bank
    close $raw
    close $output
    exit
}

after boot {
    after realtime 14 dump_f1_state
}
