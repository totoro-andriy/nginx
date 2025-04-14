# Fork Overview

This fork was developed to evaluate the real-world performance impact of using HTTP 103 Early Hints for resource preloading. It was inspired by the removal of a similar technology—HTTP/2 server push—from browsers and server implementations, which proved difficult to manage and monitor reliably.

While HTTP 103 offers a more transparent approach by letting the server suggest resources early without enforcing their delivery, our experiments show that its practical effectiveness remains limited in its current state.

This repository is a fork of the official [nginx/nginx](https://github.com/nginx/nginx), extended with support for [HTTP 103 Early Hints](https://developer.mozilla.org/en-US/docs/Web/HTTP/Status/103) based on the original work by [Roman Arutyunyan (arut)](https://github.com/arut).

## About the Arut Patch

The core Early Hints implementation is based on the patch by Roman Arutyunyan, first published in his GitHub repository:

https://github.com/arut/nginx/tree/early-hints

As of writing this (April 2025), this patch is not yet merged into the official nginx codebase. See the official NGINX blog post requesting feedback:

https://blog.nginx.org/blog/we-request-your-feedback-for-early-hints/

The original patch provides:
- Early Hints support for HTTP/1.1, HTTP/2, and HTTP/3
- Link headers injected early from existing configuration/context

### Usage (original Arut patch)

The change implements parsing Early Hints response from the proxied server and forwarding it to the client. Also, an Early Hints response could be sent to the client by the upstream module before choosing a proxied server. This happens if there are headers to send, which can be configured or added by third-party modules using the early hint filters.

A new parameter "early" is added to the "add_header" directive. The parameter instructs nginx to send this header as part of Early Hints response. The header is also sent in the main response. Also, a new directive "early_hints" is added, which accepts a predicate to enable Early Hints.

Example:

```nginx
add_header X-Foo foo early;
early_hints $http2 $http3;
proxy_pass http://bar.example.com;
```

## Additional Features in This Fork (by OVA Studio)

This fork extends Arut's patch with customizable preload logic via static text files.

### New directive

```nginx
early_hints_root /etc/nginx/early-hints;
```

### File structure and header format

```text
/etc/nginx/early-hints/
├── /global.txt
├── /global-amp.txt
├── /news/article-123.txt
├── /blog/some-entry.txt
```

There are two types of hint files:

- **Global files** (`global.txt`, `global-amp.txt`) are always applied *after* a matching custom file is found. They are used to preload assets shared across the entire site (e.g. stylesheets, JavaScript, fonts).
- **Custom files** (based on URI) are used to preload content specific to a page, such as images for an article or Open Graph previews.

If no custom file exists, nothing is returned (not even the global file).

### Header format

Hint files must contain valid `Link:` headers. Each header must be on its own line. For example:

#### Global hint file example (`global.txt`):
```text
Link: <https://example.com/assets/site.css>; rel=preload; as=style
Link: <https://example.com/assets/site.js>; rel=preload; as=script
```

#### Custom hint file example (`/news/my-article.txt`):
```text
Link: <https://example.com/media/image_w-320.avif>; rel=preload; as=image; fetchpriority=high; media=(max-width: 320px)
Link: <https://example.com/media/image_w-480.avif>; rel=preload; as=image; fetchpriority=high; media=(min-width: 321px) and (max-width: 480px)
Link: <https://example.com/media/image_w-768.avif>; rel=preload; as=image; fetchpriority=high; media=(min-width: 481px) and (max-width: 768px)
Link: <https://example.com/media/image_w-1024.avif>; rel=preload; as=image; fetchpriority=high; media=(min-width: 769px) and (max-width: 1024px)
Link: <https://example.com/media/image_w-1440.avif>; rel=preload; as=image; fetchpriority=high; media=(min-width: 1025px) and (max-width: 1440px)
Link: <https://example.com/media/image_original.avif>; rel=preload; as=image; fetchpriority=high; media=(min-width: 1441px)
```

Ensure all headers follow the `Link:` syntax as defined in the HTTP spec and that lines are not empty or malformed.

- If a `.txt` file matching the request exists, it's parsed line-by-line.
- If no custom file is found, no early hints are sent at all.
- AMP support: if the request URL includes `/amp`, global fallback is `/global-amp.txt`.
- Valid `Link:` headers are extracted and returned as 103 Early Hints.



## Comparison: Arut vs OVA Fork

| Feature                         | Arut (original) | OVA Fork        |
|-------------------------------|----------------|----------------|
| 103 Support (HTTP/1-3)         | Yes            | Yes            |
| Static preload from files      | No             | Yes            |
| AMP-aware logic                | No             | Yes            |
| Mergeable with nginx mainline | Pending        | Pending        |

## Performance Notes

This implementation currently does **not include any explicit caching**. Hint files are read from disk on every request.

However, the solution has been tested in production on endpoints serving several million unique visitors and no performance issues have been observed.

Although the module does not implement user-space caching, it relies effectively on the operating system's file system cache. The following NGINX configuration was used during testing to ensure optimal performance:

```nginx
http {
    aio threads;
    directio 4m;
    directio_alignment 4k;

    open_file_cache off;
}
```

It is recommended to monitor performance under your specific workload, especially if you serve many unique hint files per request path.

## Browser Support and Observations

Browser support for 103 Early Hints is currently rudimentary and provides no reliable mechanism to track whether a hinted resource was actually used or had any effect on load time. In our internal testing, conducted on high-traffic media content endpoints, the measured benefit was negligible or non-existent.

This may be due to the minimal gap between the 103 and the main 200 response in our setup, where backend rendering is essentially absent and static content is delivered almost instantly. As such, the browser often receives both responses in rapid succession, limiting any potential window for parallel preloading.

## Verifying 103 Headers with curl

You can check if 103 Early Hint headers are being returned correctly using `curl` with HTTP/2 support:

```bash
curl -I --http2 https://example.com/news/article.html
```

Expected output (truncated for brevity):

```http
HTTP/2 103
link: <https://example.com/assets/site.css>; rel=preload; as=style
link: <https://example.com/assets/site.js>; rel=preload; as=script
link: <https://example.com/media/image_w-768.avif>; rel=preload; as=image; fetchpriority=high; media=(min-width: 481px) and (max-width: 768px)

HTTP/2 200
...
```

This confirms the server is correctly emitting 103 Early Hints before the final HTML response.

## Related Specifications and Support

- [RFC 8297: HTTP 103 Early Hints (IETF)](https://datatracker.ietf.org/doc/html/rfc8297)
- [MDN Web Docs: HTTP 103 Early Hints](https://developer.mozilla.org/en-US/docs/Web/HTTP/Status/103)
- [Can I use: 103 Early Hints (caniuse.com)](https://caniuse.com/mdn-http_headers_status_103)

## Feedback and Contributions

This fork was created to explore real-world usage of Early Hints with media-heavy content and AMP-specific logic. Contributions, suggestions, and improvements are welcome.

