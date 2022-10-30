# asio-grpc-callback

Use the gRPC Callback API with asio. This approach is not using `grpc::CompletionQueue` like [Tradias/asio-grpc](https://github.com/Tradias/asio-grpc)
and does not provide an executor interface for the gRPC thread pool.
I think it's a viable approach and the gRPC Callback API is just way cooler than CompletionQueue.

This repository currently is in the "hacking examples" state though, not a library.
