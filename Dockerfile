FROM scratch

COPY src/main /main

USER 1000:1000

CMD ["/main"]
