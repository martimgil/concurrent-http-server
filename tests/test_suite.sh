#!/bin/bash

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color
BOLD='\033[1m'

SERVER_BIN="./bin/webserver"
PORT=8080
BASE_URL="http://localhost:$PORT"
WWW_DIR="www"
TEST_TIMEOUT=60  # Timeout in seconds

# Counter for passed and failed tests
TESTS_PASSED=0
TESTS_FAILED=0

print_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
}

print_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

# -- SETUP SERVER --
echo -e "${BOLD}Setting up server...${NC}"

echo "Compiling..."
make clean > /dev/null
make > /dev/null

if [ $? -ne 0 ]; then
    echo -e "${RED}Compilation failed. Aborting tests.${NC}"
    exit 1
fi

# Create tests files
echo "Creating test files in $WWW_DIR..."
mkdir -p $WWW_DIR
echo "<h1>Index Page</h1>" > $WWW_DIR/index.html
echo "body { color: red; }" > $WWW_DIR/style.css
echo "console.log('test');" > $WWW_DIR/script.js
echo "Fake Image Content" > $WWW_DIR/image.png

# Create server
echo -e "${BOLD}Starting server...${NC}"

if [ ! -f "$SERVER_BIN" ]; then
    echo -e "${RED}Error: Server binary not found at $SERVER_BIN${NC}"
    exit 1
fi

$SERVER_BIN & 
SERVER_PID=$!
echo "Server PID: $SERVER_PID"
sleep 2 # Wait for server to start

# Function to clean on exit
cleanup(){
    echo -e "\n${BOLD}Cleaning up...${NC}"
    if ps -p $SERVER_PID > /dev/null 2>&1; then
        kill -TERM $SERVER_PID 2>/dev/null
        # Give it 2 seconds to terminate gracefully
        sleep 2
        # If still running, force kill
        if ps -p $SERVER_PID > /dev/null 2>&1; then
            kill -9 $SERVER_PID 2>/dev/null
        fi
        wait $SERVER_PID 2>/dev/null
    fi
}
trap cleanup EXIT

# Functional tests
echo -e "\n${BOLD}Running Functional Tests - HTTP Basics ${NC}"

# Test 1: GET index.html
HTTP_CODE=$(timeout 5 curl -s -o /dev/null -w "%{http_code}" $BASE_URL/index.html)

if [ "$HTTP_CODE" -eq 200 ]; then
    print_pass "GET /index.html returned 200 OK"
else
    print_fail "GET /index.html did not return 200 OK (got $HTTP_CODE)"
fi

# Test 2: Correct Content
CONTENT=$(timeout 5 curl -s $BASE_URL/index.html)
if [[ "$CONTENT" == *"Index Page"* ]]; then
    print_pass "GET /index.html returned correct content"
else
    print_fail "GET /index.html did not return correct content"
fi

# Test 3: 404 Error
HTTP_CODE=$(timeout 5 curl -s -o /dev/null -w "%{http_code}" $BASE_URL/nonexistent.html)
if [ "$HTTP_CODE" -eq 404 ]; then
    print_pass "GET /nonexistent.html returned 404 Not Found"
else
    print_fail "GET /nonexistent.html did not return 404 Not Found (got $HTTP_CODE)"
fi

# Test Mime Types
echo -e "\n${BOLD}Testing MIME Types${NC}"

check_mime(){
    FILE=$1
    EXPECTED=$2
    TYPE=$(timeout 5 curl -s -I $BASE_URL/$FILE | grep -i "Content-Type" | awk '{print $2}' | tr -d '\r')

    if [[ "$TYPE" == *"$EXPECTED"* ]]; then
        print_pass "MIME type for $FILE is $EXPECTED"
    else
        print_fail "MIME type for $FILE is not $EXPECTED (got '$TYPE')"
    fi
}

check_mime "index.html" "text/html"
check_mime "style.css" "text/css"
check_mime "script.js" "application/javascript"
check_mime "image.png" "image/png"

# Concurrency Tests
echo -e "\n${BOLD}Testing Load (Apache Bench)${NC}"

if command -v ab >/dev/null 2>&1; then
    echo "Executing load tests (1000 requests, 100 concurrent)..."

    OUTPUT=$(timeout 30 ab -n 1000 -c 100 -k $BASE_URL/index.html 2>&1)

    if [ $? -eq 124 ]; then
        print_fail "Load test timed out"
    else
        FAILED_REQ=$(echo "$OUTPUT" | grep "Failed requests:" | awk '{print $3}')

        if [ -z "$FAILED_REQ" ]; then FAILED_REQ=0; fi

        if [ "$FAILED_REQ" -eq 0 ]; then
            print_pass "Load test passed with 0 failed requests"
        else
            print_fail "Load test failed with $FAILED_REQ failed requests"
        fi

        # Show requests per second
        RPS=$(echo "$OUTPUT" | grep "Requests per second:" | awk '{print $4}')
        echo -e "Performance: ${BOLD}$RPS requests/second${NC}"
    fi
else 
    echo -e "${RED}Apache Bench (ab) not found. Skipping load tests.${NC}"
fi

# Verify no dropped connection under load
echo -e "\n${BOLD}Verifying no dropped connections under load...${NC}"

TOTAL_REQS=10000
CONCURRENCY=100

if ! command -v ab &> /dev/null; then
    echo -e "${RED}Error: Apache Bench (ab) could not be found. Skipping dropped connection test.${NC}"
    exit 1
fi

echo "Executing load test with $TOTAL_REQS requests and concurrency of $CONCURRENCY..."

OUTPUT=$(timeout 60 ab -n $TOTAL_REQS -c $CONCURRENCY -k $BASE_URL/index.html 2>&1)

FAILED_REQS=$(echo "$OUTPUT" | grep "Failed requests:" | awk '{print $3}')
if [ -z "$FAILED_REQS" ]; then FAILED_REQS=0; fi

COMPLETE_REQS=$(echo "$OUTPUT" | grep "Complete requests:" | awk '{print $3}')
if [ -z "$COMPLETE_REQS" ]; then COMPLETE_REQS=0; fi

if [ "$FAILED_REQS" -eq 0 ] && [ "$COMPLETE_REQS" -eq "$TOTAL_REQS" ]; then
    print_pass "No dropped connections under load"
else
    print_fail "Dropped connections detected: $FAILED_REQS failed out of $TOTAL_REQS"
    echo "Complete requests: $COMPLETE_REQS"
    echo "Failed requests: $FAILED_REQS"

    echo "$OUTPUT" | grep -E "Failed requests:|Complete requests:" | sed 's/^/    /'
fi  


# Summary
echo -e "\n${BOLD}Test Summary:${NC}"
echo -e "${GREEN}Tests Passed: $TESTS_PASSED${NC}"
echo -e "${RED}Tests Failed: $TESTS_FAILED${NC}"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed${NC}"
    exit 1
fi