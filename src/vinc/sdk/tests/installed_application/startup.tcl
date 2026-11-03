# Tests catch their own assertions so failure still requests orderly shutdown.
# No Python, Node, curl or custom user main is needed to exercise the installed app.
proc httpGet {port route {token ""}} {
    set channel [socket 127.0.0.1 $port]
    fconfigure $channel -translation binary -blocking 0
    set headers "GET $route HTTP/1.1\r\nHost: 127.0.0.1:$port\r\nConnection: close\r\nX-Vinc-UI: 1\r\n"
    if {$token ne ""} { append headers "Authorization: Bearer $token\r\n" }
    puts -nonewline $channel "${headers}\r\n"
    flush $channel
    set response ""
    set deadline [expr {[clock milliseconds] + 5000}]
    while {![eof $channel]} {
        append response [read $channel]
        if {[clock milliseconds] > $deadline} {
            close $channel
            error "HTTP timeout"
        }
        after 10
    }
    close $channel
    if {![string match "HTTP/1.1 200*" $response]} { error "HTTP failure: $response" }
    return $response
}

if {[catch {
    AppSender [info nameofexecutable]
    AppReader [info nameofexecutable]
    sender identity
    reader identity
    recorder record [file join $::vinc::app_dir recorded] --all
    sender push
    set deadline [expr {[clock milliseconds] + 5000}]
    while {[reader last] != 0} {
        if {[clock milliseconds] > $deadline} { error "No custom message received" }
        after 20
    }
    # Repeated publication spans Recorder discovery without assuming exact pump timing.
    for {set sample 0} {$sample < 12} {incr sample} {
        sender loan
        after 100
    }
    recorder status
    recorder finish
    if {[file exists [file join $::vinc::app_dir vinc_data sdk-package-app ui]]} {
        set port 8080
        ui status
        set session [httpGet $port /api/session]
        if {![regexp {"token":"([^"]+)"} $session unused token]} { error "Missing UI token" }
        httpGet $port /api/ui
        set deadline [expr {[clock milliseconds] + 5000}]
        while {1} {
            set preview [httpGet $port /api/topics/preview?topic=installed%2Fcustom $token]
            if {[string match {*"state":"ready"*} $preview] && [string match {*value: *} $preview]} { break }
            if {[clock milliseconds] > $deadline} { error "Custom UI preview not ready: $preview" }
            after 30
        }
        puts "CUSTOM_UI_PREVIEW_OK"
        httpGet $port /api/graph $token
    }
    puts "APPLICATION_FLOW_OK"
} diagnostic]} {
    puts "APPLICATION_FLOW_FAILED: $diagnostic"
}
exit
