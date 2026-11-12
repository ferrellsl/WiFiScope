import java.util.Properties

// WiFiScope for Android: the Windows app's scanner, table and graphs, on a phone or tablet.
plugins {
    id("com.android.application")
}

// The key release builds are signed with: keystore.properties beside settings.gradle.kts
// (see keystore.properties.example).  Neither it nor the key is in the repository.
val keystoreFile = rootProject.file("keystore.properties")
val keystore = Properties().apply {
    if (keystoreFile.exists()) keystoreFile.inputStream().use { load(it) }
}

// The IEEE registry the Windows app builds in (../res/oui.txt), as an asset.
abstract class CopyOui : DefaultTask() {
    @get:InputFile abstract val source: RegularFileProperty
    @get:OutputDirectory abstract val output: DirectoryProperty
    @TaskAction fun copy() {
        source.get().asFile.copyTo(output.get().file("oui.txt").asFile, overwrite = true)
    }
}
val copyOui = tasks.register<CopyOui>("copyOui") {
    source.set(rootProject.layout.projectDirectory.file("../res/oui.txt"))
}
androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(copyOui, CopyOui::output)
    }
}

android {
    namespace = "com.ferrellsl.wifiscope"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.ferrellsl.wifiscope"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.2"
    }
    signingConfigs {
        if (keystoreFile.exists()) {
            create("release") {
                storeFile = file(keystore.getProperty("storeFile"))
                storePassword = keystore.getProperty("storePassword")
                keyAlias = keystore.getProperty("keyAlias")
                keyPassword = keystore.getProperty("keyPassword")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            if (keystoreFile.exists()) signingConfig = signingConfigs.getByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
