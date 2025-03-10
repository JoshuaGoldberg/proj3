REPORT

This design follows pretty much what the assignment specifies. The main design decision was to open new TCP connections for sending and receiving, as it made it easier to work with peers that may not be active yet (such as peers that start after a set delay). Additionlly, heartbeats were set up as specifiedm using UDP. I decided to utilize threads as I wanted to avoid potentially confusing behaviour if I handled UDP and TCP connects at the same time without using threads. As a result, I had to use a lot of mutex locking and unlocking to prevent weird behaviour with peers sometimes misreading membership lists and crash detections when membership lists updated via the TCP logic. The main issues while implementing were dealing with recv blocking/causing issues with crash detection. I resolved this by having the leader send itself a TCP message to specify a crash (or leader failure), which allowed recv to not block (as it would receive the CRASH message and begin a deletion protocol). Beyond that, the use of new TCP connections made it pretty simple to handle the main logic loop, as I just set it up to connect to hosts in the membership list, send REQ messages when specified, collect OK messages from peers, and send NEWVIEW messages. The specifications also say to provide scripts. I wasn't too sure what that meant, but here are the commands to run all 4 testfiles:

test1 (membership):
docker compose -f docker-compose-testcase-1.yml up

test2 (crash detection):
docker compose -f docker-compose-testcase-2.yml up

test3 (proper member deletion):
docker compose -f docker-compose-testcase-3.yml up

test4 (leader failure resumes properly):
docker compose -f docker-compose-testcase-4.yml up