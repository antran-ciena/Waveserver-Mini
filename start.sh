#!/bin/bash
 
make
if [ $? -ne 0 ]; then
    echo "Build failed — exiting"
    exit 1
fi
 
echo "Starting services..."
 
./port_manager &
PORT_MGR_PID=$!
 
./conn_manager &
CONN_MGR_PID=$!
 
./traffic_manager &
TRAFFIC_PID=$!
 
./protection_manager &
PROT_PID=$!
 
echo ""
echo "All services running. Press Ctrl+C to stop."
echo "Logs: tail -f wsmini.log"
 
trap "echo 'Stopping...'; kill $PORT_MGR_PID $CONN_MGR_PID $TRAFFIC_PID $PROT_PID; exit 0" SIGINT
 
wait
 