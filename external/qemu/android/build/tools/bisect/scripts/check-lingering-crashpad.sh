#!/bin/bash

# 1. pkill -9 crashpad_handler
echo "Cleaning up any existing crashpad_handler processes..."
pkill -9 crashpad_handler 2>/dev/null

# 2. execute the command $1
if [ -z "$1" ]; then
	echo "Usage: $0 <command>"
	exit 1
fi

echo "Executing command: $1"
# Use exec within the background shell to ensure APP_PID is the emulator process itself
eval "exec $1" &
APP_PID=$!

# 3. wait 30 seconds
echo "Waiting 30 seconds for initialization..."
sleep 30

# 4. send the terminate (kill) to the proc launches by $1
echo "Sending SIGTERM to process $APP_PID..."
kill -TERM "$APP_PID" 2>/dev/null

# 5. wait for at most 5 seconds for this process to end.
echo "Waiting up to 15 seconds for process to exit..."
for i in {1..15}; do
	if ! kill -0 "$APP_PID" 2>/dev/null; then
		echo "Process exited."
		break
	fi
	sleep 1
done

# 6. If it does not kill -9.
if kill -0 "$APP_PID" 2>/dev/null; then
	echo "Process did not exit, sending SIGKILL..."
	kill -9 "$APP_PID" 2>/dev/null
	wait "$APP_PID" 2>/dev/null
fi

# 7. wait for at most 120 seconds for crashpad_handler to disappear.
for i in {120..1}; do
	if ! pgrep crashpad_handler >/dev/null; then
		echo "Success: No crashpad_handler found."
		exit 0
	fi
	echo "Waiting up to $i more seconds for crashpad_handler to disappear..."
	sleep 1
done

# 8. If it is still there, kill it and exit 1.
echo "Failure: crashpad_handler is still alive after 120 seconds."
pkill -9 crashpad_handler 2>/dev/null
exit 1
