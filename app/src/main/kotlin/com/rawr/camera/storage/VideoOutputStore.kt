package com.rawr.camera.storage

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.MediaStore
import androidx.core.net.toUri

/** Provider operations for pending MediaStore videos and user-selected document trees. */
internal class VideoOutputStore(private val context: Context) {
    fun create(saveLocationId: String, displayName: String, capturedAt: Long, mimeType: String = "video/mp4"): Uri {
        val destination = CaptureSaveLocation.fromId(saveLocationId, CaptureSaveLocation.VIDEO_ROOTS)
        val resolver = context.contentResolver
        val tree = destination.treeUri?.toUri()
        if (destination.relativePath.isNotEmpty()) {
            val values = ContentValues().apply {
                put(MediaStore.Video.Media.DISPLAY_NAME, displayName)
                put(MediaStore.Video.Media.MIME_TYPE, mimeType)
                put(MediaStore.Video.Media.RELATIVE_PATH, destination.relativePath)
                put(MediaStore.Video.Media.IS_PENDING, 1)
                put(MediaStore.Video.Media.DATE_TAKEN, capturedAt)
            }
            // A picked folder's volume may be unmounted; its tree grant is the fallback.
            val inserted = if (tree == null) resolver.insert(destination.videoCollection(), values)
                else runCatching { resolver.insert(destination.videoCollection(), values) }.getOrNull()
            if (inserted != null || tree == null) return checkNotNull(inserted) {
                "Could not create video in ${destination.relativePath}"
            }
        }
        val parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        return checkNotNull(DocumentsContract.createDocument(resolver, parent, mimeType, displayName)) {
            "Could not create video in the selected folder"
        }
    }

    /**
     * A pending, non-media file (an .mcraw clip) beside the video. MediaStore only
     * admits video MIME types into the video collection and may refuse other files
     * under media folders, so the generic Files collection is tried at the video's
     * folder first and at Documents/RAWR otherwise. A picked folder gets it directly.
     */
    fun createRaw(saveLocationId: String, displayName: String, capturedAt: Long): Uri {
        val destination = CaptureSaveLocation.fromId(saveLocationId, CaptureSaveLocation.VIDEO_ROOTS)
        val resolver = context.contentResolver
        val tree = destination.treeUri?.toUri()
        if (destination.relativePath.isNotEmpty()) {
            val files = MediaStore.Files.getContentUri(destination.volumeName)
            for (path in listOf(destination.relativePath, "${Environment.DIRECTORY_DOCUMENTS}/RAWR/")) {
                val values = ContentValues().apply {
                    put(MediaStore.MediaColumns.DISPLAY_NAME, displayName)
                    put(MediaStore.MediaColumns.MIME_TYPE, RAW_MIME_TYPE)
                    put(MediaStore.MediaColumns.RELATIVE_PATH, path)
                    put(MediaStore.MediaColumns.IS_PENDING, 1)
                    put(MediaStore.MediaColumns.DATE_TAKEN, capturedAt)
                }
                runCatching { resolver.insert(files, values) }.getOrNull()?.let { return it }
            }
            checkNotNull(tree) { "Could not create $displayName in ${destination.relativePath} or Documents/RAWR" }
        }
        val parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        return checkNotNull(DocumentsContract.createDocument(resolver, parent, RAW_MIME_TYPE, displayName)) {
            "Could not create $displayName in the selected folder"
        }
    }

    fun publish(uri: Uri) {
        // Document providers have no MediaStore pending flag. Closing the
        // native descriptor finalizes their file at the selected destination.
        if (uri.authority == MediaStore.AUTHORITY) {
            check(context.contentResolver.update(uri,
                ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }, null, null) > 0) {
                "Video output disappeared before publication"
            }
        }
    }

    companion object {
        /** MotionCam .mcraw has no registered MIME type. */
        const val RAW_MIME_TYPE = "application/octet-stream"
    }

    fun delete(uri: Uri) {
        if (uri.authority == MediaStore.AUTHORITY) context.contentResolver.delete(uri, null, null)
        else check(DocumentsContract.deleteDocument(context.contentResolver, uri)) {
            "Could not remove unfinished video"
        }
    }
}
