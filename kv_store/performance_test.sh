#!/bin/bash

# Make sure server is running before executing this script.

echo "--- Starting Performance Test ---"
echo "Creating 100 concurrent clients doing SET operations..."

start_time=$(date +%s%N)

for i in {1..100}
do
   ./client SET key$i val$i > /dev/null &
done

# Wait for all background jobs to finish
wait

end_time=$(date +%s%N)
elapsed=$((($end_time - $start_time)/1000000))
echo "100 SET operations completed in $elapsed ms."

echo "Creating 100 concurrent clients doing GET operations..."

start_time=$(date +%s%N)

for i in {1..100}
do
   ./client GET key$i > /dev/null &
done

wait

end_time=$(date +%s%N)
elapsed=$((($end_time - $start_time)/1000000))
echo "100 GET operations completed in $elapsed ms."

echo "--- Performance Test Finished ---"
