# This fixture checks Publisher defaults, independently of UI bind availability.
if {[catch {
    param set vinc.pub_default_slots 3
    param set vinc.pub_default_message_bytes 8192
    if {[PublisherDefaultsProbe] != 0} { error "Probe startup failed" }
    if {[probe original 3 8192] != 0} { error "Original capacity mismatch" }
    param set vinc.pub_default_slots 5
    if {[probe current 5 8192] != 0} { error "Live capacity mismatch" }
    if {[probe original 3 8192] != 0} { error "Original Publisher was resized" }
    param set vinc.pub_default_slots 0
    if {[probe current 5 8192] != 0} { error "Last effective settings not restored" }
    param save
    puts "PUBLISHER_DEFAULTS_SDK_OK"
} diagnostic]} {
    puts "PUBLISHER_DEFAULTS_SDK_FAILED: $diagnostic"
}
exit
