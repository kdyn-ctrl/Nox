FROM ubuntu:24.04

# Install build dependencies
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    cmake \
    libcurl4-openssl-dev \
    wget \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 1. DOWNLOAD THE HEADERS DIRECTLY INTO THE BUILD CONTAINER
ADD https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h .
RUN mkdir -p nlohmann
ADD https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp ./nlohmann/json.hpp

# 2. Copy your local source files
COPY main.cpp RegimeStateMachine.hpp ./

# 3. Compile the Analyst Agent
RUN g++ -O3 -std=c++17 main.cpp -o analyst_agent

# Run the compiled binary
CMD ["./analyst_agent"]
