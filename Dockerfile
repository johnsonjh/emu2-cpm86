FROM alpine:3.20 AS builder
RUN apk add --no-cache build-base
WORKDIR /src
COPY . .
RUN make

FROM alpine:3.20
COPY --from=builder /src/emu2 /usr/local/bin/emu2
ENTRYPOINT ["emu2"]
