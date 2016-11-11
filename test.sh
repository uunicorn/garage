
sequence=11111111111111111111111111111111111111111111111111111111111111

# Set 40.685MHz carrier frequency
echo 40685000 > /sys/devices/platform/garage-door/carrier

# Set sample rate to 1250Hz (800us period)
echo 1250 > /sys/devices/platform/garage-door/srate

# Send the sequence
echo "$sequence" > /sys/devices/platform/garage-door/sequence 

