package com.plzsayyes3.kyf44memo;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.os.Bundle;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private EditText memo;
    private TextView status;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        memo = findViewById(R.id.memo);
        status = findViewById(R.id.status);

        findViewById(R.id.send).setOnClickListener(v -> capture());
        findViewById(R.id.settings).setOnClickListener(v -> showSettings());

        memo.requestFocus();
        MemoSyncWorker.enqueue(this);

        if (SecretStore.getEndpoint(this).isEmpty() || SecretStore.getToken(this).isEmpty()) {
            showSettings();
        } else {
            updatePendingStatus("入力できます。");
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        MemoSyncWorker.enqueue(this);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getAction() == KeyEvent.ACTION_UP && event.getKeyCode() == KeyEvent.KEYCODE_CALL) {
            capture();
            return true;
        }
        if (event.getAction() == KeyEvent.ACTION_UP && event.getKeyCode() == KeyEvent.KEYCODE_MENU) {
            showSettings();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private void capture() {
        String text = memo.getText().toString();
        if (text.trim().isEmpty()) {
            Toast.makeText(this, "メモが空です", Toast.LENGTH_SHORT).show();
            return;
        }
        if (SecretStore.getEndpoint(this).isEmpty() || SecretStore.getToken(this).isEmpty()) {
            showSettings();
            return;
        }

        try {
            MemoStore.savePending(this, text);
        } catch (IOException e) {
            status.setText("端末への保存に失敗しました");
            return;
        }

        memo.setText("");
        memo.requestFocus();
        updatePendingStatus("端末に保存しました。送信します…");
        MemoSyncWorker.enqueue(this);

        executor.execute(() -> {
            try {
                int sent = MemoStore.syncAll(getApplicationContext());
                runOnUiThread(() -> updatePendingStatus(sent > 0 ? "送信しました" : "送信待ちはありません"));
            } catch (Exception e) {
                runOnUiThread(() -> updatePendingStatus("オフライン保存済み。通信時に再送します"));
            }
        });
    }

    private void showSettings() {
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(pad, pad / 2, pad, 0);

        EditText endpoint = new EditText(this);
        endpoint.setHint("https://receiver.example.com");
        endpoint.setSingleLine(true);
        endpoint.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        endpoint.setText(SecretStore.getEndpoint(this));

        EditText token = new EditText(this);
        token.setHint("KYF44_DEVICE_TOKEN");
        token.setSingleLine(true);
        token.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        token.setText(SecretStore.getToken(this));

        box.addView(endpoint);
        box.addView(token);

        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Receiver設定")
                .setView(box)
                .setNegativeButton("キャンセル", null)
                .setPositiveButton("保存", null)
                .create();

        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String receiver = endpoint.getText().toString().trim();
            String deviceToken = token.getText().toString().trim();
            if (!receiver.startsWith("https://") || deviceToken.isEmpty()) {
                Toast.makeText(this, "HTTPS URLと端末トークンが必要です", Toast.LENGTH_SHORT).show();
                return;
            }

            try {
                SecretStore.putEndpoint(this, receiver);
                SecretStore.putToken(this, deviceToken);
                dialog.dismiss();
                updatePendingStatus("設定を保存しました");
                MemoSyncWorker.enqueue(this);
                memo.requestFocus();
                ((InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE))
                        .showSoftInput(memo, InputMethodManager.SHOW_IMPLICIT);
            } catch (Exception e) {
                Toast.makeText(this, "設定保存に失敗しました", Toast.LENGTH_SHORT).show();
            }
        }));
        dialog.show();
    }

    private void updatePendingStatus(String prefix) {
        int pending = MemoStore.pendingCount(this);
        status.setText(prefix + "  未送信: " + pending);
    }

    @Override
    protected void onDestroy() {
        executor.shutdownNow();
        super.onDestroy();
    }
}
