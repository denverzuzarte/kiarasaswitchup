#!/usr/bin/env python3
"""fake_llm.py - canned OpenAI-compatible server for testing intern.py
without a model. Answers cycle through: a SEARCH that matches nothing, a
patch that breaks the results (must fail a gate), and a harmless correct
patch (must reach the benchmark). Usage: fake_llm.py [port]"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

ANSWERS = [
    # 1. SEARCH text that does not exist
    """I add a prefetch.
<<<<<<< SEARCH src/dict.c
this line does not exist anywhere
=======
nothing
>>>>>>> REPLACE""",
    # 2. compiles, but picks the LARGEST name instead of the smallest: must fail a gate
    """Use > instead of < (wrong on purpose).
<<<<<<< SEARCH src/dict.c
        if (bytes_cmp(x.ptr, x.len, m.ptr, m.len) < 0)
            aux[r] = i;
=======
        if (bytes_cmp(x.ptr, x.len, m.ptr, m.len) > 0)
            aux[r] = i;
>>>>>>> REPLACE""",
    # 3. harmless and correct: a prefetch in the final loop
    """Prefetch the link slot 16 iterations ahead in the final loop.
<<<<<<< SEARCH src/dict.c
    for (uint32_t i = 0; i < ids; i++)
        if (lb->dkey[i].k1)
            lb->slots[lb->dslot[i]].val = lb->dkey[aux[dsu_find(parent, i)]];
=======
    for (uint32_t i = 0; i < ids; i++) {
        if (i + 16 < ids)
            __builtin_prefetch(&lb->slots[lb->dslot[i + 16]], 1);
        if (lb->dkey[i].k1)
            lb->slots[lb->dslot[i]].val = lb->dkey[aux[dsu_find(parent, i)]];
    }
>>>>>>> REPLACE""",
]
n = 0


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        global n
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        if "senior performance engineer" in req["messages"][0]["content"]:   # brainstorm
            content = "\n".join("%d. fake idea number %d for this function, keep it identical" % (i, i)
                                for i in range(1, 6))
        else:
            content = ANSWERS[n % len(ANSWERS)]
            n += 1
        body = json.dumps({"choices": [{"message": {"role": "assistant", "content": content}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


HTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8099), H).serve_forever()
