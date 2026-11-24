package app.nativespotify

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

internal class Secrets(context: Context) {
    private val prefs = context.getSharedPreferences("secrets", Context.MODE_PRIVATE)
    private val alias = "native-spotify-aes"

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
            generateKey()
        }
    }

    @Synchronized
    fun get(name: String): String? = runCatching {
        val stored = prefs.getString(name, null) ?: return null
        val bytes = Base64.decode(stored, Base64.NO_WRAP)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, bytes, 0, 12))
        cipher.updateAAD(name.toByteArray())
        cipher.doFinal(bytes, 12, bytes.size - 12).decodeToString()
    }.getOrNull()

    @Synchronized
    fun put(name: String, value: String?, durable: Boolean = false) {
        if (value == null) {
            val edit = prefs.edit().remove(name)
            if (durable) {
                check(edit.commit())
            } else {
                edit.apply()
            }
            return
        }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        check(cipher.iv.size == 12)
        cipher.updateAAD(name.toByteArray())
        val encrypted = cipher.iv + cipher.doFinal(value.toByteArray())
        val edit = prefs.edit().putString(name, Base64.encodeToString(encrypted, Base64.NO_WRAP))
        if (durable) {
            check(edit.commit())
        } else {
            edit.apply()
        }
    }
}
