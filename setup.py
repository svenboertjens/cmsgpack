from setuptools import setup, Extension

ext_modules = [
    Extension(
        'cmsgpack.cmsgpack',
        sources=[
            'cmsgpack/cmsgpack.c',
        ],
        include_dirs=[
            'cmsgpack/'
        ],
    )
]

setup(
    name="cmsgpack",
    version="0.0.4",
    
    author="Sven Boertjens",
    author_email="boertjens.sven@gmail.com",
    
    description="High-performance MessagePack library written in C",
    
    long_description=open('README', 'r').read(),
    long_description_content_type="text/markdown",
    
    url="https://github.com/svenboertjens/cmsgpack",
    
    packages=['cmsgpack'],
    install_requires=[],
    
    ext_modules=ext_modules,
    include_package_data=True,
    package_data={
        'cmsgpack': [
            '*.pyi',
            '*.h',
        ]
    },
    
    classifiers=[
        "License :: OSI Approved :: Apache Software License",
        "Operating System :: OS Independent",
    ],
    license='MIT',
)
