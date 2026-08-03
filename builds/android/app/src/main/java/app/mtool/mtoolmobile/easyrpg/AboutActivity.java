package app.mtool.mtoolmobile.easyrpg;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import org.easyrpg.player.R;

public class AboutActivity extends AppCompatActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.about_splash);

        TextView versionText = findViewById(R.id.version_text);
        try {
            PackageInfo packageInfo = getPackageManager().getPackageInfo(getPackageName(), 0);
            versionText.setText(getString(
                    R.string.version_info,
                    packageInfo.versionName,
                    packageInfo.versionCode
            ));
        } catch (PackageManager.NameNotFoundException x) {
            versionText.setText("");
        }
    }
}
