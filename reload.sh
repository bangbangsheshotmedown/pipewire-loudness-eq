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
    pactl unload-module module-null-sink 2>/dev/null
    
    exit 0
}

# Trap Ctrl+C and exit
trap cleanup SIGINT SIGTERM EXIT

# 1. Save current default sink
ORIGINAL_SINK=$(pactl get-default-sink 2>/dev/null)
if [ -z "$ORIGINAL_SINK" ]; then
    echo "Error: Could not determine current default sink."
    exit 1
fi
echo "Physical Output: $ORIGINAL_SINK"

# 2. Create Virtual Sink (Null Sink)
echo "Creating Virtual Sink..."
pactl unload-module module-null-sink 2>/dev/null

MODULE_ID=$(pactl load-module module-null-sink \
    sink_name=$SINK_NAME \
    sink_properties="device.description='$SINK_DESC'")

if [ -z "$MODULE_ID" ]; then
    echo "Error: Could not create virtual sink."
    exit 1
fi
echo "Virtual Sink created with ID: $MODULE_ID"

# Try to link volume using pw-metadata (may not work on all systems)
NODE_ID=$(pw-dump Node | grep -B 20 "$SINK_NAME" | grep "id" | head -n 1 | awk '{print $2}' | tr -d ',')
if [ -n "$NODE_ID" ]; then
    echo "Linking volume for Node ID: $NODE_ID"
    pw-metadata -n settings 0 "node.link-volume=$NODE_ID:true" 2>/dev/null
fi

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
for i in {1..50}; do
    if pw-link -o | grep -q "loudness-eq:output_FL"; then
        break
    fi
    sleep 0.1
done

# 6. Establish the Links
echo "Establishing routing..."

# Find Physical Sink Ports
PHYS_PORTS_L=$(pw-link -i | grep "$ORIGINAL_SINK" | grep -E "playback_FL|playback_0|playback.L" | head -n 1)
PHYS_PORTS_R=$(pw-link -i | grep "$ORIGINAL_SINK" | grep -E "playback_FR|playback_1|playback.R" | head -n 1)

# Find Virtual Sink Monitor Ports
VIRT_PORTS_L=$(pw-link -o | grep "$SINK_NAME" | grep -E "monitor_FL|monitor_0|monitor.L" | head -n 1)
VIRT_PORTS_R=$(pw-link -o | grep "$SINK_NAME" | grep -E "monitor_FR|monitor_1|monitor.R" | head -n 1)

echo "Linking $VIRT_PORTS_L -> loudness-eq:input_FL"
pw-link "$VIRT_PORTS_L" "loudness-eq:input_FL"
echo "Linking $VIRT_PORTS_R -> loudness-eq:input_FR"
pw-link "$VIRT_PORTS_R" "loudness-eq:input_FR"

echo "Linking loudness-eq:output_FL -> $PHYS_PORTS_L"
pw-link "loudness-eq:output_FL" "$PHYS_PORTS_L"
echo "Linking loudness-eq:output_FR -> $PHYS_PORTS_R"
pw-link "loudness-eq:output_FR" "$PHYS_PORTS_R"

echo "-------------------------------------------------------"
echo "SYSTEM-WIDE LOUDNESS EQUALIZATION ACTIVE."
echo "Routing: [Apps] -> $SINK_NAME -> [Equalizer] -> $ORIGINAL_SINK"
echo "-------------------------------------------------------"

# Stay alive to keep the plugin and sink active
wait $LOUD_PID
