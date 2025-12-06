#!/bin/bash

# =============================================================================
# Extended Stress Test Script (5+ minutes continuous load)
# Requirement 21: Run for 5+ minutes with continuous load
# =============================================================================

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color
BOLD='\033[1m'

# Configuration
SERVER_BIN="./bin/webserver"
PORT=8080
BASE_URL="http://127.0.0.1:$PORT"
WWW_DIR="www"

echo -e "${BOLD}Extended Stress Test (5+ minutes continuous load)${NC}"
echo "============================================================================="

# Check if Apache Bench is available
if ! command -v ab &> /dev/null; then
    echo -e "${RED}Error: Apache Bench (ab) not found. Please install apache2-utils.${NC}"
    exit 1
fi

# Check if server is running
if ! curl -s -o /dev/null -w "%{http_code}" "$BASE_URL/index.html" | grep -q "200"; then
    echo "Server not running. Starting server..."
    
    # Compile and start server
    make clean > /dev/null 2>&1
    make > /dev/null 2>&1
    
    # Create test files
    mkdir -p "$WWW_DIR"
    echo "<h1>Index Page</h1>" > "$WWW_DIR/index.html"
    
    $SERVER_BIN > server.log 2>&1 &
    SERVER_PID=$!
    echo "Server started with PID: $SERVER_PID"
    sleep 5
    
    # Verify server is running
    if ! curl -s -o /dev/null -w "%{http_code}" "$BASE_URL/index.html" | grep -q "200"; then
        echo -e "${RED}Failed to start server${NC}"
        exit 1
    fi
    
    STARTED_SERVER=1
else
    echo "Server already running"
    STARTED_SERVER=0
fi

echo ""
echo "Running 5-minute continuous load test..."
echo "Parameters: ab -t 300 -c 50 (300 seconds, 50 concurrent connections)"
echo "This will take approximately 5 minutes. Please wait..."
echo ""

START_TIME=$(date +%s)
DURATION=300
END_TIME=$((START_TIME + DURATION))

TOTAL_COMPLETE=0
TOTAL_FAILED=0

echo "Stress test started at $(date)"
echo "Target duration: ${DURATION} seconds"

while [ $(date +%s) -lt $END_TIME ]; do
    CURRENT_TIME=$(date +%s)
    REMAINING=$((END_TIME - CURRENT_TIME))
    
    # Safety check
    if [ $REMAINING -le 0 ]; then break; fi
    
    echo "Running load batch... (~$REMAINING seconds remaining)"
    
    # Run ab with remaining time.
    # Set -n very high (50 million) so -t (time) is the likely limiting factor.
    # Even if it finishes early (high throughput), the loop will restart it.
    OUTPUT=$(ab -t $REMAINING -n 50000000 -c 50 -r -k "$BASE_URL/index.html" 2>&1)
    
    BATCH_COMPLETE=$(echo "$OUTPUT" | grep "Complete requests:" | awk '{print $3}')
    BATCH_FAILED=$(echo "$OUTPUT" | grep "Failed requests:" | awk '{print $3}')
    
    BATCH_COMPLETE=${BATCH_COMPLETE:-0}
    BATCH_FAILED=${BATCH_FAILED:-0}
    
    TOTAL_COMPLETE=$((TOTAL_COMPLETE + BATCH_COMPLETE))
    TOTAL_FAILED=$((TOTAL_FAILED + BATCH_FAILED))
    
    # Brief pause to let system breathe and prevent tight loop if ab fails instantly
    sleep 0.2
done

ACTUAL_DURATION=$(( $(date +%s) - START_TIME ))
if [ $ACTUAL_DURATION -le 0 ]; then ACTUAL_DURATION=1; fi

FAILED_REQS=$TOTAL_FAILED
COMPLETE_REQS=$TOTAL_COMPLETE
# Calculate average RPS using awk
REQUESTS_PER_SEC=$(awk -v c=$TOTAL_COMPLETE -v t=$ACTUAL_DURATION 'BEGIN {printf "%.2f", c/t}')

echo ""
echo "============================================================================="
echo -e "${BOLD}Results (Total Duration: ${ACTUAL_DURATION}s):${NC}"
echo "  Completed requests: $TOTAL_COMPLETE"
echo "  Failed requests:    $TOTAL_FAILED"
echo "  Requests/second:    $REQUESTS_PER_SEC (avg)"

# Calculate failure rate
if [ "$COMPLETE_REQS" -gt 0 ]; then
    FAILURE_RATE=$(echo "scale=4; $FAILED_REQS * 100 / $COMPLETE_REQS" | bc)
else
    FAILURE_RATE=100
fi

# Determine pass/fail (allow up to 1% failure rate)
if [ "$FAILED_REQS" -eq 0 ]; then
    echo -e "${GREEN}[PASS] Extended stress test passed with 0 failures${NC}"
    RESULT=0
elif [ $(echo "$FAILURE_RATE < 1" | bc) -eq 1 ]; then
    echo -e "${GREEN}[PASS] Extended stress test passed (failure rate: ${FAILURE_RATE}% < 1%)${NC}"
    RESULT=0
else
    echo -e "${RED}[FAIL] Extended stress test failed (failure rate: ${FAILURE_RATE}%)${NC}"
    RESULT=1
fi

# Cleanup if we started the server
if [ "$STARTED_SERVER" -eq 1 ]; then
    echo ""
    echo "Stopping server..."
    kill -15 $SERVER_PID 2>/dev/null
    sleep 2
    kill -9 $SERVER_PID 2>/dev/null
fi

exit $RESULT
