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

# Run the extended load test
OUTPUT=$(ab -t 300 -c 50 -r -k "$BASE_URL/index.html" 2>&1)

# Parse results
FAILED_REQS=$(echo "$OUTPUT" | grep "Failed requests:" | awk '{print $3}')
COMPLETE_REQS=$(echo "$OUTPUT" | grep "Complete requests:" | awk '{print $3}')
REQUESTS_PER_SEC=$(echo "$OUTPUT" | grep "Requests per second:" | awk '{print $4}')
TIME_PER_REQ=$(echo "$OUTPUT" | grep "Time per request:" | head -1 | awk '{print $4}')

# Default to 0 if empty
FAILED_REQS=${FAILED_REQS:-0}
COMPLETE_REQS=${COMPLETE_REQS:-0}

echo ""
echo "============================================================================="
echo -e "${BOLD}Results:${NC}"
echo "  Completed requests: $COMPLETE_REQS"
echo "  Failed requests:    $FAILED_REQS"
echo "  Requests/second:    $REQUESTS_PER_SEC"
echo "  Time per request:   ${TIME_PER_REQ}ms"
echo ""

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
