package app.nativespotify

import android.app.Activity
import android.app.Dialog
import android.content.Context
import android.net.Uri
import android.util.Base64
import android.view.ViewGroup
import android.webkit.CookieManager
import android.webkit.RenderProcessGoneDetail
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import java.io.ByteArrayInputStream
import java.security.MessageDigest
import java.security.SecureRandom
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

internal class PlaybackWebGrant(val code: String, val verifier: String)

/** A provider-controlled, single-origin HTTPS code flow; not a Web Player session. */
internal class PlaybackWebEndpoints(val signIn: Uri, val refresh: Uri, val callback: Uri) {
    private val host = requireNotNull(callback.host)

    init {
        require(listOf(signIn, refresh, callback).all {
            allows(it) && !it.encodedPath.isNullOrBlank() && it.encodedQuery == null && it.fragment == null
        })
        require(callback.encodedPath != signIn.encodedPath && callback.encodedPath != refresh.encodedPath)
    }

    fun allows(uri: Uri): Boolean = uri.scheme == "https" &&
        (uri.encodedAuthority == host || uri.encodedAuthority == "$host:443")
}

internal object PlaybackWebSession {
    suspend fun signIn(activity: Activity, endpoints: PlaybackWebEndpoints): PlaybackWebGrant =
        authorize(activity, endpoints, activity)

    suspend fun refresh(context: Context, endpoints: PlaybackWebEndpoints): PlaybackWebGrant =
        authorize(context.applicationContext, endpoints, null)

    private suspend fun authorize(context: Context, endpoints: PlaybackWebEndpoints, activity: Activity?):
        PlaybackWebGrant = withContext(Dispatchers.Main.immediate) {
        if (activity?.isFinishing == true || activity?.isDestroyed == true) {
            throw LocalPlaybackUnavailable("Playback sign-in screen is closed")
        }

        val state = randomValue()
        val verifier = randomValue()
        val challenge = base64Url(MessageDigest.getInstance("SHA-256")
            .digest(verifier.toByteArray(Charsets.US_ASCII)))
        val result = CompletableDeferred<PlaybackWebGrant>()
        val dialog = activity?.let { Dialog(it) }
        val web = WebView(context)
        var rendererGone = false

        try {
            web.settings.allowFileAccess = false
            web.settings.allowContentAccess = false
            web.settings.mixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW
            CookieManager.getInstance().setAcceptThirdPartyCookies(web, false)

            web.webViewClient = object : WebViewClient() {
                override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                    if (!request.isForMainFrame || !endpoints.allows(request.url)) {
                        return true
                    }

                    val url = request.url
                    if (url.encodedPath != endpoints.callback.encodedPath) {
                        return false
                    }

                    val code = url.getQueryParameters("code").singleOrNull()
                    val valid = url.getQueryParameters("state").singleOrNull() == state &&
                        url.fragment == null && url.getQueryParameter("error") == null

                    if (valid && !code.isNullOrBlank()) {
                        result.complete(PlaybackWebGrant(code, verifier))
                    } else {
                        result.completeExceptionally(LocalPlaybackUnavailable("Playback sign-in required or callback invalid"))
                    }
                    return true
                }

                override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse? {
                    if (endpoints.allows(request.url)) {
                        return null
                    }

                    return WebResourceResponse("text/plain", "UTF-8", ByteArrayInputStream(byteArrayOf()))
                }

                override fun onReceivedError(view: WebView, request: WebResourceRequest, error: WebResourceError) {
                    if (request.isForMainFrame) {
                        result.completeExceptionally(LocalPlaybackUnavailable("Playback authentication failed to load"))
                    }
                }

                override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
                    rendererGone = true
                    result.completeExceptionally(LocalPlaybackUnavailable("Playback authentication renderer stopped"))
                    return true
                }
            }

            dialog?.setContentView(web)
            dialog?.setOnDismissListener { result.cancel() }
            dialog?.show()

            val endpoint = if (activity == null) endpoints.refresh else endpoints.signIn
            web.loadUrl(endpoint.buildUpon()
                .appendQueryParameter("state", state)
                .appendQueryParameter("code_challenge", challenge)
                .appendQueryParameter("code_challenge_method", "S256")
                .build().toString())

            val grant = withTimeoutOrNull(if (activity == null) 15_000L else 180_000L) { result.await() }
                ?: throw LocalPlaybackUnavailable("Playback authentication timed out")

            withContext(Dispatchers.IO) { CookieManager.getInstance().flush() }
            grant
        } finally {
            result.cancel()
            if (!rendererGone) {
                web.stopLoading()
            }

            dialog?.setOnDismissListener(null)
            (web.parent as? ViewGroup)?.removeView(web)
            dialog?.dismiss()
            web.destroy()
        }
    }

    private fun randomValue(): String = base64Url(ByteArray(32).also { SecureRandom().nextBytes(it) })

    private fun base64Url(bytes: ByteArray): String =
        Base64.encodeToString(bytes, Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP)
}
