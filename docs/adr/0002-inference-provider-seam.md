# Keep model inference behind an asynchronous semantic seam

Inference uses a private `start`, nonblocking `poll`, and `task_free`
interface. Requests carry a bounded list of named, role-based byte inputs and
one exact source clock. They also pin the provider name, provider version, and
model hash. Results use event-bundle terms rather than model tensors.

Each adapter owns model loading, audio preparation, tensor layout, limits,
and result decoding. A shared check validates every result bundle and binds
each payload path once to bytes with the stated size and hash. The caller owns
unchanged request data until `task_free`; the provider owns stable task and
result data until then. `task_free` cancels and clears one task. A separate
provider destroy callback clears shared model or worker state after all tasks
have ended.

This design lets native code finish at once and lets web code finish after an
asynchronous run. We rejected the synchronous experiment renderer interface
because browser model calls return later. We also rejected raw tensor input
and output because a model change could then change the analyzer interface.
Process launch and saved job files remain separate native adapter work. The
interface stays private until a fixed adapter and two real runtime adapters
agree on it.

The in-memory check enforces row, work, per-payload, and total payload-byte
caps. It cannot know the exact manifest and JSONL sizes. The save bridge
streams each bound byte source into a new event bundle, then uses the bundle
reader to enforce the exact saved-file caps and hashes.

The event bundle does not yet record the request task, seed, or hashes for
non-source context. We will define saved run provenance before a real provider
ships. Provider identity and source-audio identity alone do not provide it.
