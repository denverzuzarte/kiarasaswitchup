# Minimal runtime image. src/main must be a statically linked linux/amd64 ELF.
FROM scratch

COPY src/main /main

USER 1000:1000
WORKDIR /
CMD ["/main"]
