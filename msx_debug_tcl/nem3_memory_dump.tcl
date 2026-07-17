set nem3_dump_path "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/nem3-openmsx-dump-current.txt"

proc dump_nem3_memory {} {
    global nem3_dump_path
    set output [open $nem3_dump_path w]
    puts $output "PC=[format %04X [reg PC]]"
    puts $output "SP=[format %04X [reg SP]]"
    puts $output "AF=[format %04X [reg AF]]"
    puts $output "BC=[format %04X [reg BC]]"
    puts $output "DE=[format %04X [reg DE]]"
    puts $output "HL=[format %04X [reg HL]]"
    puts $output "RAMAD1=[format %02X [debug read memory 0xF342]]"
    puts $output "RAMAD2=[format %02X [debug read memory 0xF343]]"
    puts $output "RAMAD3=[format %02X [debug read memory 0xF344]]"
    puts $output "PHASE=[format %02X [debug read memory 0xC600]]"
puts $output "MAPPER_FD=[format %02X [debug read ioports 0xFD]]"
puts $output "MAPPER_FE=[format %02X [debug read ioports 0xFE]]"
puts $output "MAPPER_FF=[format %02X [debug read ioports 0xFF]]"
    puts $output "HTIMI_HEX=[binary encode hex [debug read_block memory 0xFD9F 0x05]]"
    puts $output "PLAY_HEX=[binary encode hex [debug read_block memory 0x5FE0 0x20]]"
    puts $output "ENGINE_HEX=[binary encode hex [debug read_block memory 0x5FC0 0x40]]"
    puts $output "PLAYER_HEX=[binary encode hex [debug read_block memory 0xC000 0x40]]"
    puts $output "E000_HEX=[binary encode hex [debug read_block memory 0xE000 0x40]]"
    puts $output "F000_HEX=[binary encode hex [debug read_block memory 0xF000 0x40]]"
    puts $output "CPU_8000_HEX=[binary encode hex [debug read_block memory 0x8000 0x20]]"
    puts $output "DEBUG_LIST=[debug list]"
    set page1 [debug read_block memory 0x7F9B 0x65]
    set staged [debug read_block memory 0xD100 0x65]
    puts $output "MEM_7F9B_HEX=[binary encode hex $page1]"
    puts $output "MEM_D100_HEX=[binary encode hex $staged]"
    puts $output "MAIN_RAM_SIZE=[debug size {Main RAM}]"
    puts $output "MAIN_RAM_DESC=[debug desc {Main RAM}]"
    foreach segment {0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15} {
        set address [expr {$segment * 0x4000 + 0x3F9B}]
        set data [debug read_block {Main RAM} $address 0x65]
        puts $output "MAIN_RAM_SEGMENT_${segment}=[binary encode hex $data]"
    }
    set main_ram [debug read_block {Main RAM} 0 [debug size {Main RAM}]]
    set signature [binary format H* "14e8fffe01ee01e907ea07eb0370d1b0"]
    puts $output "MAIN_RAM_SIGNATURE_OFFSET=[string first $signature $main_ram]"
    set raw [open "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/nem3-mainram.bin" w]
    fconfigure $raw -translation binary -encoding binary
    puts -nonewline $raw $main_ram
    close $raw
    close $output
}

after boot {
    after realtime 10 {
        type "RUN \"AUTOEXEC.BAS\"\r"
        after realtime 20 {
            dump_nem3_memory
            exit
        }
    }
}
