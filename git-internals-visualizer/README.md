# Inside Git

A dependency-free interactive lab for explaining Git internals at interview depth. It models the working tree, index, object database, refs, reflog, commit graph, object serialization, and packfile delta representation.

## Run locally

```sh
npm start
```

Then open `http://localhost:4173`.

- `index.html` is the Git internals edition.
- `jj.html` is the Jujutsu internals edition.

## What is real vs simulated

- Object IDs in the object workbench are computed in-browser from Git's canonical `type + size + NUL + payload` byte sequence using SHA-1.
- The repository lab is a deterministic model of Git state transitions. OIDs are shortened teaching fixtures so the graph stays readable.
- Pack layouts and index fields follow Git's documented formats, but byte sizes in the animated example are illustrative.

Primary references in this repository:

- `Documentation/gitformat-pack.adoc`
- `Documentation/gitformat-index.adoc`
- `Documentation/technical/hash-function-transition.adoc`
