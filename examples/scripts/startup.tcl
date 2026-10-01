# Run from the repository root:
#   .\build\windows-msvc-debug\Vinc.exe -S examples/scripts/startup.tcl
# This script leaves the framework running when deployment is complete.
puts "Starting exampleModule..."
exampleModule
exampleNode hello
puts "Deployment complete. Use help, or exampleNode status."
