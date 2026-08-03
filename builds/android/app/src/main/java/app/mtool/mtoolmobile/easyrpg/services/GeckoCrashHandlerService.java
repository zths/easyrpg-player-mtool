package app.mtool.mtoolmobile.easyrpg.services;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import org.mozilla.geckoview.GeckoRuntime;

import app.mtool.mtoolmobile.easyrpg.PackageContextHelper;
import org.easyrpg.player.R;

public class GeckoCrashHandlerService extends Service {
    private static final String TAG = "GeckoCrashHandler";
    private static final String CHANNEL_ID = "app.mtool.mtoolmobile.easyrpg.gecko_crash_handler";
    private static final int NOTIFICATION_ID = 1002;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startAsForegroundService();

        try {
            persistCrash(intent);
            Log.e(TAG, "Saved pending Gecko crash details");
        } catch (Exception e) {
            Log.e(TAG, "Failed to save Gecko crash details", e);
        }

        stopForeground(true);
        stopSelf(startId);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void startAsForegroundService() {
        createNotificationChannel();

        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }

        Notification notification = builder
                .setSmallIcon(R.drawable.ic_launcher)
                .setContentTitle("Handling Gecko crash")
                .setContentText("Saving crash details...")
                .setOngoing(true)
                .build();

        startForeground(NOTIFICATION_ID, notification);
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }

        NotificationManager notificationManager = getSystemService(NotificationManager.class);
        if (notificationManager == null) {
            return;
        }

        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "Gecko Crash Handler",
                NotificationManager.IMPORTANCE_LOW
        );
        channel.setDescription("Used to save Gecko crash details for the next launch.");
        channel.setSound(null, null);
        channel.enableVibration(false);
        channel.setShowBadge(false);
        notificationManager.createNotificationChannel(channel);
    }

    private void persistCrash(Intent intent) {
        if (intent == null) {
            return;
        }

        StringBuilder content = new StringBuilder();
        appendLine(content, "timestamp", Long.toString(System.currentTimeMillis()));
        appendLine(content, "processType", intent.getStringExtra(GeckoRuntime.EXTRA_CRASH_PROCESS_TYPE));
        appendLine(content, "processVisibility", intent.getStringExtra(GeckoRuntime.EXTRA_CRASH_PROCESS_VISIBILITY));
        appendLine(content, "remoteType", intent.getStringExtra(GeckoRuntime.EXTRA_CRASH_REMOTE_TYPE));
        appendLine(content, "minidumpPath", intent.getStringExtra(GeckoRuntime.EXTRA_MINIDUMP_PATH));
        appendLine(content, "extrasPath", intent.getStringExtra(GeckoRuntime.EXTRA_EXTRAS_PATH));
        PackageContextHelper.writeFileToOtherApp(this, "gecko/pending_crash.txt", content.toString());
    }

    private void appendLine(StringBuilder builder, String key, String value) {
        builder.append(key)
                .append("=")
                .append(sanitize(value))
                .append("\n");
    }

    private String sanitize(String value) {
        if (value == null) {
            return "";
        }
        return value.replace("\r", " ").replace("\n", " ");
    }
}
