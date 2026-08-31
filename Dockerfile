FROM alpine:3.20 AS builder
ARG GIT_SHA=unknown
RUN apk add --no-cache build-base
WORKDIR /src
COPY . .
RUN SHORT="${GIT_SHA%"${GIT_SHA#???????}"}" && \
    make EXTRA_CFLAGS="-Wall -g -Werror=implicit-function-declaration -DGIT_SHA='\"${SHORT}\"'"

FROM alpine:3.20
COPY --from=builder /src/emu2 /usr/local/bin/emu2
ENTRYPOINT ["emu2"]
