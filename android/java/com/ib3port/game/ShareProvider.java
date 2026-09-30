package com.ib3port.game;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * Lends the files the launcher shares (the log report) to the app the player picks in the share
 * sheet, read only: content://<package>.share/<name> is the file <cache>/share/<name>.
 */
public class ShareProvider extends ContentProvider {
    static File folder(android.content.Context context) {
        return new File(context.getCacheDir(), "share");
    }

    static Uri uriFor(android.content.Context context, File file) {
        return new Uri.Builder().scheme("content").authority(context.getPackageName() + ".share")
                .appendPath(file.getName()).build();
    }

    private File fileFor(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        File dir = folder(getContext());
        File f = new File(dir, name == null ? "" : name);
        try {
            if (name == null || !f.getCanonicalFile().getParentFile().equals(dir.getCanonicalFile()) || !f.isFile())
                throw new FileNotFoundException(uri.toString());
        } catch (IOException e) {
            throw new FileNotFoundException(uri.toString());
        }
        return f;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileFor(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        File f;
        try {
            f = fileFor(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        if (projection == null) projection = new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        Object[] row = new Object[projection.length];
        for (int i = 0; i < projection.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(projection[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(projection[i])) row[i] = f.length();
        }
        MatrixCursor c = new MatrixCursor(projection, 1);
        c.addRow(row);
        return c;
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        throw new UnsupportedOperationException();
    }
}
