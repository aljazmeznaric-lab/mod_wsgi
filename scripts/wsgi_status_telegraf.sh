#!/bin/bash
#
# Telegraf exec input plugin script for mod_wsgi status monitoring
# 
# Usage in telegraf.conf:
#   [[inputs.exec]]
#     commands = ["/path/to/wsgi_status_telegraf.sh"]
#     timeout = "5s"
#     data_format = "influx"
#
# Environment variables:
#   WSGI_STATUS_URL - URL to wsgi-status endpoint (default: http://localhost/wsgi-status)
#   WSGI_STATUS_HOST - hostname tag for metrics (default: hostname)
#

set -e

# Configuration
WSGI_STATUS_URL="${WSGI_STATUS_URL:-http://localhost/wsgi-status}"
HOST_TAG="${WSGI_STATUS_HOST:-$(hostname)}"

# Fetch the status JSON
JSON=$(curl -s --max-time 5 "$WSGI_STATUS_URL" 2>/dev/null)

if [ -z "$JSON" ]; then
    echo "wsgi_status,host=$HOST_TAG error=1i,message=\"failed_to_fetch\"" 
    exit 0
fi

# Check for error in response
if echo "$JSON" | jq -e '.error' > /dev/null 2>&1; then
    echo "wsgi_status,host=$HOST_TAG error=1i"
    exit 0
fi

# Get timestamp (convert to nanoseconds for InfluxDB)
TIMESTAMP=$(echo "$JSON" | jq -r '.timestamp')
TIMESTAMP_NS=$(echo "$TIMESTAMP * 1000000000" | bc | cut -d'.' -f1)

# Count active requests per pool
declare -A pool_counts
declare -A pool_processing
declare -A pool_queued

# Parse active requests
REQUESTS=$(echo "$JSON" | jq -c '.active_requests[]' 2>/dev/null || echo "")

TOTAL_REQUESTS=0
TOTAL_PROCESSING=0
TOTAL_QUEUED=0

if [ -n "$REQUESTS" ]; then
    while IFS= read -r req; do
        REQUEST_ID=$(echo "$req" | jq -r '.request_id')
        POOL_NAME=$(echo "$req" | jq -r '.pool_name')
        WORKER_ID=$(echo "$req" | jq -r '.worker_id')
        THREAD_ID=$(echo "$req" | jq -r '.thread_id')
        PID=$(echo "$req" | jq -r '.pid')
        URI=$(echo "$req" | jq -r '.uri')
        METHOD=$(echo "$req" | jq -r '.method')
        STATUS=$(echo "$req" | jq -r '.status')
        START_TIME=$(echo "$req" | jq -r '.start_time')
        DURATION=$(echo "$req" | jq -r '.duration')
        
        # Escape special characters in URI for InfluxDB tags
        URI_ESCAPED=$(echo "$URI" | sed 's/,/\\,/g; s/ /\\ /g; s/=/\\=/g')
        
        # Output individual request metrics
        echo "wsgi_request,host=$HOST_TAG,pool=$POOL_NAME,worker_id=$WORKER_ID,thread_id=$THREAD_ID,pid=$PID,method=$METHOD,status=$STATUS duration=$DURATION,start_time=$START_TIME $TIMESTAMP_NS"
        
        # Track counts
        TOTAL_REQUESTS=$((TOTAL_REQUESTS + 1))
        pool_counts[$POOL_NAME]=$((${pool_counts[$POOL_NAME]:-0} + 1))
        
        if [ "$STATUS" = "processing" ]; then
            TOTAL_PROCESSING=$((TOTAL_PROCESSING + 1))
            pool_processing[$POOL_NAME]=$((${pool_processing[$POOL_NAME]:-0} + 1))
        elif [ "$STATUS" = "queued" ]; then
            TOTAL_QUEUED=$((TOTAL_QUEUED + 1))
            pool_queued[$POOL_NAME]=$((${pool_queued[$POOL_NAME]:-0} + 1))
        fi
        
    done <<< "$REQUESTS"
fi

# Output summary metrics
echo "wsgi_status,host=$HOST_TAG active_requests=${TOTAL_REQUESTS}i,processing=${TOTAL_PROCESSING}i,queued=${TOTAL_QUEUED}i,error=0i $TIMESTAMP_NS"

# Output per-pool metrics
for pool in "${!pool_counts[@]}"; do
    COUNT=${pool_counts[$pool]}
    PROCESSING=${pool_processing[$pool]:-0}
    QUEUED=${pool_queued[$pool]:-0}
    echo "wsgi_pool,host=$HOST_TAG,pool=$pool active_requests=${COUNT}i,processing=${PROCESSING}i,queued=${QUEUED}i $TIMESTAMP_NS"
done

