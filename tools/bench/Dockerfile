FROM ubuntu:24.04
RUN apt-get update -qq \
  && apt-get install -y -qq --no-install-recommends iproute2 tcpdump libyaml-cpp0.8 >/dev/null \
  && rm -rf /var/lib/apt/lists/*
