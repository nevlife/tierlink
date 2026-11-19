# Built from the repository root, so the shared wire crate is in the context.
FROM rust:1.95-trixie AS build
WORKDIR /src
COPY wire wire
COPY server/Cargo.toml server/Cargo.lock server/
COPY server/src server/src
RUN cargo build --release --locked --manifest-path server/Cargo.toml

FROM debian:trixie-slim
COPY --from=build /src/server/target/release/fleetd /usr/local/bin/fleetd
ENTRYPOINT ["fleetd"]
CMD ["/etc/fleetd/fleetd.yaml"]
