#!/bin/bash

# --- Configuration ---
SINK_NAME="Loudness_Equalizer"
SINK_DESC="Loudness Equalizer (Virtual)"

# --- Cleanup Function ---
cleanup() {
    echo -e "\nRestoring original settings..."
    
    # Restore the original default sink first
    if [ -n "$ORIGINAL_SINK" ]; then
        echo "Restoring sink to: $ORIGINAL_SINK"
        pactl set-default-sink "$ORIGINAL_SINK" 2>/dev/null
    fi
    
    # Kill the plugin
    pkill -f loudness-eq 2>/dev/null
    
    # Unload the specific module
    if [ -n "$MODULE_ID" ]; then
        echo "Unloading virtual sink (ID: $MODULE_ID)..."
        pactl unload-module "$MODULE_ID" 2>/dev/null
    else
        # Fallback: unload all null sinks with our name if ID was missed
        pactl unload-module module-null-sink 2>/dev/null
    fi
    
    exit 0
}

# Trap Ctrl+C and exit
trap cleanup SIGINT SIGTERM EXIT

# 1. Save current default sink
ORIGINAL_SINK=$(pactl get-default-sink 2>/dev/null)
echo "Physical Output: $ORIGINAL_SINK"

# 2. Create Virtual Sink (Null Sink)
echo "Creating Virtual Sink..."
# Try to unload any existing ones first to be clean
pactl unload-module module-null-sink 2>/dev/null

MODULE_ID=$(pactl load-module module-null-sink \
    sink_name=$SINK_NAME \
    sink_properties=device.description="$SINK_DESC")

if [ -z "$MODULE_ID" ]; then
    echo "Error: Could not create virtual sink."
    exit 1
fi
echo "Virtual Sink created with ID: $MODULE_ID"

# 3. Set it as default
echo "Setting $SINK_NAME as default output..."
pactl set-default-sink $SINK_NAME

# 4. Compile and Start Plugin
make || exit 1
echo "Starting Loudness Equalizer..."
./loudness-eq &
LOUD_PID=$!

# 5. Wait for ports to appear
echo "Waiting for ports..."
for i in {1..20}; do
    if pw-link -o | grep -q "loudness-eq:output_FL"; then
        break
    fi
    sleep 0.1
done

if ! pw-link -o | grep -q "loudness-eq:output_FL"; then
    echo "Error: loudness-eq ports did not appear."
    cleanup
fi

# 6. Establish the Links
echo "Establishing routing..."

# Link Virtual Sink Monitor -> Filter Input
# (The Null Sink has a monitor port that gives us the audio being played to it)
pw-link "$SINK_NAME:monitor_FL" "loudness-eq:input_FL" 2>/dev/null
pw-link "$SINK_NAME:monitor_FR" "loudness-eq:input_FR" 2>/dev/null

# Link Filter Output -> Physical Speakers
pw-link "loudness-eq:output_FL" "$ORIGINAL_SINK:playback_FL" 2>/dev/null
pw-link "loudness-eq:output_FR" "$ORIGINAL_SINK:playback_FR" 2>/dev/null

echo "-------------------------------------------------------"
echo "SYSTEM-WIDE LOUDNESS EQUALIZATION ACTIVE."
echo "All audio is now being routed through the equalizer."
echo "Settings: (Edit loudness.c or use env vars to tune)"
echo "-------------------------------------------------------"

# Stay alive to keep the plugin and sink active
wait $LOUD_PID
