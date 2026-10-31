FROM gcc:16@sha256:43978d7710a803d1c3d32f4117c1528ec7285a4ce2c0a51e982ec1a6e3d87d95 AS build
COPY . /w/
RUN gcc -O3 -flto -march=native -pthread -o /w/c /w/main.c
FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
COPY --from=build /w/c /c
ENTRYPOINT ["/c"]
