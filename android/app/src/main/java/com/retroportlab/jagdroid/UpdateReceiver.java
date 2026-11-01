package com.retroportlab.jagdroid;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.widget.Toast;

/** Result of an update install: shows the system confirmation screen, or reports why it failed. */
public class UpdateReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context ctx, Intent intent) {
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                ctx.startActivity(confirm);
            }
            return;
        }
        if (status == PackageInstaller.STATUS_SUCCESS) return;   // the app is replaced and relaunched by the system
        String msg;
        switch (status) {
            case PackageInstaller.STATUS_FAILURE_ABORTED: msg = "Update cancelled"; break;
            case PackageInstaller.STATUS_FAILURE_CONFLICT:
            case PackageInstaller.STATUS_FAILURE_INCOMPATIBLE:
                msg = "This update is signed differently from the installed JagDroid. Install it manually from the releases page.";
                break;
            case PackageInstaller.STATUS_FAILURE_STORAGE: msg = "Not enough storage to install the update"; break;
            default:
                String detail = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                msg = "Update failed" + (detail != null ? ": " + detail : "");
        }
        Toast.makeText(ctx, msg, Toast.LENGTH_LONG).show();
    }
}
