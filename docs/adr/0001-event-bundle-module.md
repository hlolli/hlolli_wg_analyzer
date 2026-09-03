# Keep event bundles behind one owned aggregate

The event-bundle module owns read, full validation, write, and free through
one aggregate interface; analysis and model adapters infer facts before they
cross this seam. This keeps schema, path, hash, link, and lifetime rules in one
place while leaving internal file streaming free to change. We rejected a
one-shot audio-to-score compiler because it would join unrelated policies,
and we rejected a large public streaming interface because it would expose
file order and partial-write state to every caller.
