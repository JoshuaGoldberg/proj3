FROM ubuntu:22.04

RUN apt-get update
RUN apt-get install -y gcc
RUN apt-get install -y netcat iputils-ping

ADD proj3.c /app/
ADD proj3.h /app/
ADD hostsfile.txt /app/
WORKDIR /app/
RUN gcc proj3.c -o proj3


ENTRYPOINT ["/app/proj3"]