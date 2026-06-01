FROM alpine:latest AS builder
RUN apk add --no-cache g++ make cmake openssl-dev build-base

WORKDIR /app
# Download the single-header libraries
ADD https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h .
ADD https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp .

COPY . .
# Compile the engine
RUN g++ -O3 main.cpp -o openclaw_engine -lssl -lcrypto -lpthread

FROM alpine:latest
RUN apk add --no-cache libstdc++ libgcc libssl3 libcrypto3
WORKDIR /app
COPY --from=builder /app/openclaw_engine .
EXPOSE 8080
CMD ["./openclaw_engine"]
