package app.mtool.mtoolmobile.easyrpg;

import android.content.Context;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;

public class PackageContextHelper {
    private static final String FILE_PROVIDER_AUTHORITY = "app.mtool.fileprovider";

    public static Context getContext(Context context) {
        try {
            return context.createPackageContext("app.mtool.mtoolmobile", Context.CONTEXT_IGNORE_SECURITY);
        } catch (PackageManager.NameNotFoundException e) {
            e.printStackTrace();
            return null;
        }
    }

    public static String readFileFromOtherApp(Context context, String fileName) {
        Uri fileUri = Uri.parse("content://app.mtool.fileprovider/file/" + fileName);
        try (ParcelFileDescriptor pfd = context.getContentResolver().openFileDescriptor(fileUri, "r")) {
            FileInputStream fis = new FileInputStream(pfd.getFileDescriptor());
            byte[] buffer = new byte[4096];
            StringBuilder content = new StringBuilder();
            int len;
            while ((len = fis.read(buffer)) != -1) {
                content.append(new String(buffer, 0, len));
            }
            return content.toString();
        } catch (Exception e) {
            Log.i("ContentProviderAccess", "读取文件失败", e);
            return null;
        }
    }

    public static boolean writeFileToOtherApp(Context context, String fileName, String content) {
        Uri fileUri = Uri.parse("content://app.mtool.fileprovider/file/" + fileName);
        try (ParcelFileDescriptor pfd = context.getContentResolver().openFileDescriptor(fileUri, "w")) {
            FileOutputStream fos = new FileOutputStream(pfd.getFileDescriptor());
            fos.write(content.getBytes());
            fos.flush();
            return true;
        } catch (Exception e) {
            Log.i("ContentProviderAccess", "写入文件失败: " + fileName, e);
            return false;
        }
    }

    public static boolean checkFileExistsByOpening(Context context, String fileName) {
        Uri fileUri = Uri.parse("content://app.mtool.fileprovider/file/" + fileName);
        try {
            ParcelFileDescriptor pfd = context.getContentResolver().openFileDescriptor(fileUri, "r");
            if (pfd != null) {
                pfd.close();
                return true;
            }
            return false;
        } catch (FileNotFoundException e) {
            return false;
        } catch (IOException e) {
            Log.i("FileCheck", "检查文件存在时关闭描述符出错", e);
            return false;
        }
    }

    public static ParcelFileDescriptor openFile(Context context, String fileName, String mode) throws FileNotFoundException {
        Uri fileUri = Uri.parse("content://app.mtool.fileprovider/file/" + fileName);
        return context.getContentResolver().openFileDescriptor(fileUri, mode);
    }

    public static boolean isFileExistInProvider(Context context, String fileName) {
        Uri uri = Uri.parse("content://app.mtool.fileprovider/exists/" + fileName);

        try (Cursor cursor = context.getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int exists = cursor.getInt(cursor.getColumnIndex("exists"));
                return exists == 1;
            }
            return false;
        } catch (Exception e) {
            Log.i("FileCheck", "检查文件存在时出错", e);
            return false;
        }
    }

    public static boolean deleteFileFromOtherApp(Context context, String fileName) {
        Uri fileUri = Uri.parse("content://app.mtool.fileprovider/file/" + fileName);
        try {
            int deletedRows = context.getContentResolver().delete(fileUri, null, null);
            return deletedRows > 0;
        } catch (Exception e) {
            Log.i("ContentProviderAccess", "删除文件失败", e);
            return false;
        }
    }
}
