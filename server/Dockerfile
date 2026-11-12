FROM rust:1.95-trixie AS build
WORKDIR /src
COPY Cargo.toml Cargo.lock ./
COPY src src
RUN cargo build --release --locked

FROM debian:trixie-slim
COPY --from=build /src/target/release/fleetd /usr/local/bin/fleetd
ENTRYPOINT ["fleetd"]
CMD ["/etc/fleetd/fleetd.yaml"]
