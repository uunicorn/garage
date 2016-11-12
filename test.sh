
# the code from switches in the remote
code=111111001110

# encode the carrier on/off sequence
sequence=$(echo -n "0${code}0" | tr '01' '10' | sed -e 's/./10&/g' -e 's/$/11111/')

# repeat 5 times with some leading 1's to give receiver time to warm up,
# and a trailing 0 just to make sure we turn off the carrier
sequence=1111${sequence}${sequence}${sequence}${sequence}${sequence}0

# Set 40.685MHz carrier frequency
echo 40685000 > /sys/devices/platform/garage-door/carrier

# Set sample rate to 1250Hz (800us period)
echo 1250 > /sys/devices/platform/garage-door/srate

# Send the sequence
echo "$sequence" > /sys/devices/platform/garage-door/sequence 

