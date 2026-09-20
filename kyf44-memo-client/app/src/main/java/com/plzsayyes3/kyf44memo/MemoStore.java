package com.plzsayyes3.kyf44memo;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.UUID;

import javax.net.ssl.HttpsURLConnection;
import java.net.URL;

final class MemoStore {
    private static final String QUEUE_DIR = "pending-memos";

    private MemoStore() {}

    static String savePending(Context context, String text) throws IOException {
        File dir = queueDir(context);
        if (!dir.exists() && !dir.mkdirs()) {
            throw new IOException("Could not create pending memo directory");
        }

        String stamp = new SimpleDateFormat("yyyyMMddHHmmssSSS", Locale.JAPAN).format(new Date());
        String suffix = UUID.randomUUID().toString().replace("-", "").substring(0, 8);
        String memoId = stamp + "-" + suffix;
        File target = new File(dir, memoId + ".txt");

        try (FileOutputStream out = new FileOutputStream(target)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.flush();
            out.getFD().sync();
        }
        return memoId;
    }

    static int pendingCount(Context context) {
        File[] files = queueDir(context).listFiles((dir, name) -> name.endsWith(".txt"));
        return files == null ? 0 : files.length;
    }

    static synchronized int syncAll(Context context) throws IOException {
        String endpoint = normalizeEndpoint(SecretStore.getEndpoint(context));
        String token = SecretStore.getToken(context);
        if (endpoint.isEmpty() || token.isEmpty()) {
            throw new IllegalStateException("Receiver settings are incomplete");
        }

        File[] files = queueDir(context).listFiles((dir, name) -> name.endsWith(".txt"));
        if (files == null || files.length == 0) {
            return 0;
        }
        Arrays.sort(files);

        int sent = 0;
        for (File file : files) {
            byte[] data = java.nio.file.Files.readAllBytes(file.toPath());
            String memoId = file.getName().substring(0, file.getName().length() - 4);

            HttpsURLConnection connection = (HttpsURLConnection) new URL(endpoint).openConnection();
            connection.setConnectTimeout(15000);
            connection.setReadTimeout(20000);
            connection.setRequestMethod("POST");
            connection.setDoOutput(true);
            connection.setRequestProperty("Authorization", "Bearer " + token);
            connection.setRequestProperty("X-Memo-ID", memoId);
            connection.setRequestProperty("Content-Type", "text/plain; charset=utf-8");
            connection.setFixedLengthStreamingMode(data.length);

            try {
                connection.getOutputStream().write(data);
                int code = connection.getResponseCode();
                if (code == 200 || code == 201) {
                    if (!file.delete()) {
                        throw new IOException("Memo sent but local queue file could not be removed");
                    }
                    sent++;
                    continue;
                }
                throw new IOException("Receiver returned HTTP " + code);
            } finally {
                connection.disconnect();
            }
        }
        return sent;
    }

    private static File queueDir(Context context) {
        return new File(context.getFilesDir(), QUEUE_DIR);
    }

    private static String normalizeEndpoint(String raw) {
        String endpoint = raw == null ? "" : raw.trim();
        if (endpoint.isEmpty()) {
            return "";
        }
        while (endpoint.endsWith("/")) {
            endpoint = endpoint.substring(0, endpoint.length() - 1);
        }
        if (!endpoint.startsWith("https://")) {
            throw new IllegalStateException("HTTPS receiver URL is required");
        }
        if (endpoint.endsWith("/v1/recordings")) {
            endpoint = endpoint.substring(0, endpoint.length() - "/v1/recordings".length());
        }
        if (!endpoint.endsWith("/v1/memos")) {
            endpoint += "/v1/memos";
        }
        return endpoint;
    }
}
