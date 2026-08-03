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

        try {
            PackageInfo packageInfo = getPackageManager().getPackageInfo(getPackageName(), 0);
            TextView versionText = findViewById(R.id.version_text);
            versionText.setText(getString(
                    R.string.version_info,
                    packageInfo.versionName,
                    packageInfo.versionCode
            ));

            TextView attributionText = findViewById(R.id.attribution_text);
            attributionText.setText(getString(R.string.project_attribution));

            TextView appNameText = findViewById(R.id.app_name_text);
            appNameText.setText(getString(R.string.app_name));
        } catch (PackageManager.NameNotFoundException x) {
            findViewById(R.id.version_text).setVisibility(android.view.View.GONE);
        }
    }
}
