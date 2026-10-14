# The framework is already initialized. This is the application's deployment flow.
puts "Starting MyApp from $::vinc::app_dir"
exampleModule --name myExample
myExample hello
# Returning leaves the framework and myExample running.
